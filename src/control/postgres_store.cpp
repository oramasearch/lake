// The one translation unit that names libpqxx. Each call opens (or reuses) one
// connection and runs one transaction, inline on the caller's thread; SQL
// errors map to the store's error codes.
#include "lake/control/postgres_store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <liquid/identity.hpp>

#include "lake/control/identity_crypto.hpp"

#include <pqxx/pqxx>

#include "lake/control/policy.hpp"

namespace lake::control {

namespace identity_crypto {
namespace {

std::string hex(const unsigned char* bytes, std::size_t count) {
    std::string result;
    result.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        result += std::format("{:02x}", static_cast<unsigned>(bytes[index]));
    }
    return result;
}

std::vector<unsigned char> from_hex(std::string_view value) {
    if (value.size() % 2 != 0)
        return {};
    std::vector<unsigned char> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        unsigned parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data() + index, value.data() + index + 2, parsed, 16);
        if (error != std::errc{} || end != value.data() + index + 2)
            return {};
        result.push_back(static_cast<unsigned char>(parsed));
    }
    return result;
}

std::string base64url(const unsigned char* bytes, std::size_t count) {
    std::string encoded(((count + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), bytes, static_cast<int>(count));
    if (size < 0) {
        return {};
    }
    encoded.resize(static_cast<std::size_t>(size));
    while (encoded.ends_with('=')) {
        encoded.pop_back();
    }
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::vector<unsigned char> from_base64url(std::string_view value) {
    std::string encoded(value);
    std::replace(encoded.begin(), encoded.end(), '-', '+');
    std::replace(encoded.begin(), encoded.end(), '_', '/');
    while (encoded.size() % 4 != 0) {
        encoded.push_back('=');
    }
    std::vector<unsigned char> result((encoded.size() / 4) * 3);
    const int size = EVP_DecodeBlock(result.data(),
                                     reinterpret_cast<const unsigned char*>(encoded.data()),
                                     static_cast<int>(encoded.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (encoded.ends_with('=')) {
        ++padding;
    }
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') {
        ++padding;
    }
    result.resize(static_cast<std::size_t>(size) - padding);
    return result;
}

bool audience_contains(const nlohmann::json& audience, std::string_view client_id) {
    if (audience.is_string()) {
        return audience.get<std::string>() == client_id;
    }
    if (!audience.is_array()) {
        return false;
    }
    return std::any_of(audience.begin(), audience.end(), [&](const nlohmann::json& entry) {
        return entry.is_string() && entry.get<std::string>() == client_id;
    });
}

EVP_PKEY* rsa_key_from_jwk(const nlohmann::json& jwk) {
    if (jwk.value("kty", "") != "RSA") {
        return nullptr;
    }
    const std::vector<unsigned char> modulus = from_base64url(jwk.value("n", ""));
    const std::vector<unsigned char> exponent = from_base64url(jwk.value("e", ""));
    if (modulus.empty() || exponent.empty()) {
        return nullptr;
    }
    BIGNUM* n = BN_bin2bn(modulus.data(), static_cast<int>(modulus.size()), nullptr);
    BIGNUM* e = BN_bin2bn(exponent.data(), static_cast<int>(exponent.size()), nullptr);
    if (n == nullptr || e == nullptr) {
        BN_free(n);
        BN_free(e);
        return nullptr;
    }
    OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
    if (builder == nullptr || OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) != 1) {
        OSSL_PARAM_BLD_free(builder);
        BN_free(n);
        BN_free(e);
        return nullptr;
    }
    OSSL_PARAM* parameters = OSSL_PARAM_BLD_to_param(builder);
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* key = nullptr;
    if (parameters == nullptr || context == nullptr || EVP_PKEY_fromdata_init(context) != 1 ||
        EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters) != 1) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    EVP_PKEY_CTX_free(context);
    OSSL_PARAM_free(parameters);
    OSSL_PARAM_BLD_free(builder);
    BN_free(n);
    BN_free(e);
    return key;
}

std::vector<unsigned char> sha256(std::span<const unsigned char> value) {
    std::vector<unsigned char> digest(EVP_MD_get_size(EVP_sha256()));
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    digest.resize(size);
    return digest;
}

std::vector<unsigned char> sha256(std::string_view value) {
    return sha256(std::span(reinterpret_cast<const unsigned char*>(value.data()), value.size()));
}

std::uint32_t network_uint32(std::span<const unsigned char> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

class CborReader {
public:
    explicit CborReader(std::span<const unsigned char> bytes) : bytes_(bytes) {}

    std::optional<std::size_t> map_size() {
        return length_for_major(5);
    }

    std::optional<long long> integer() {
        if (position_ >= bytes_.size()) {
            return std::nullopt;
        }
        const unsigned char initial = bytes_[position_++];
        const unsigned major = initial >> 5;
        if (major != 0 && major != 1) {
            return std::nullopt;
        }
        const auto magnitude = length(initial & 0x1f);
        if (!magnitude || *magnitude > static_cast<std::uint64_t>(LLONG_MAX)) {
            return std::nullopt;
        }
        return major == 0 ? static_cast<long long>(*magnitude)
                          : -1 - static_cast<long long>(*magnitude);
    }

    std::optional<std::vector<unsigned char>> byte_string() {
        const auto size = length_for_major(2);
        if (!size || *size > bytes_.size() - position_) {
            return std::nullopt;
        }
        std::vector<unsigned char> result(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                                          bytes_.begin() +
                                              static_cast<std::ptrdiff_t>(position_ + *size));
        position_ += *size;
        return result;
    }

    [[nodiscard]] bool finished() const {
        return position_ == bytes_.size();
    }

private:
    std::optional<std::size_t> length_for_major(unsigned expected) {
        if (position_ >= bytes_.size()) {
            return std::nullopt;
        }
        const unsigned char initial = bytes_[position_++];
        if ((initial >> 5) != expected) {
            return std::nullopt;
        }
        const auto result = length(initial & 0x1f);
        if (!result || *result > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(*result);
    }

    std::optional<std::uint64_t> length(unsigned additional) {
        if (additional < 24) {
            return additional;
        }
        std::size_t count = 0;
        if (additional == 24) {
            count = 1;
        } else if (additional == 25) {
            count = 2;
        } else if (additional == 26) {
            count = 4;
        } else if (additional == 27) {
            count = 8;
        } else {
            return std::nullopt;
        }
        if (count > bytes_.size() - position_) {
            return std::nullopt;
        }
        std::uint64_t result = 0;
        for (std::size_t index = 0; index < count; ++index) {
            result = (result << 8) | bytes_[position_++];
        }
        return result;
    }

    std::span<const unsigned char> bytes_;
    std::size_t position_ = 0;
};

std::expected<std::vector<unsigned char>, std::string>
verified_client_data(std::string_view encoded,
                     std::string_view expected_type,
                     std::string_view challenge,
                     std::string_view origin) {
    const std::vector<unsigned char> bytes = from_base64url(encoded);
    if (bytes.empty()) {
        return std::unexpected("WebAuthn client data is not valid base64url");
    }
    const nlohmann::json document = nlohmann::json::parse(bytes, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::unexpected("WebAuthn client data type, challenge, or origin is invalid");
    }
    const bool cross_origin_invalid =
        document.contains("crossOrigin") &&
        (!document["crossOrigin"].is_boolean() || document["crossOrigin"].get<bool>());
    if (!document.contains("type") || !document["type"].is_string() ||
        document["type"].get<std::string>() != expected_type || !document.contains("challenge") ||
        !document["challenge"].is_string() ||
        document["challenge"].get<std::string>() != challenge || !document.contains("origin") ||
        !document["origin"].is_string() || document["origin"].get<std::string>() != origin ||
        cross_origin_invalid) {
        return std::unexpected("WebAuthn client data type, challenge, or origin is invalid");
    }
    return bytes;
}

bool verified_authenticator_scope(std::span<const unsigned char> auth_data,
                                  std::string_view rp_id) {
    if (auth_data.size() < 37 || (auth_data[32] & 0x01) == 0) {
        return false;
    }
    const std::vector<unsigned char> expected_hash = sha256(rp_id);
    return expected_hash.size() == 32 &&
           CRYPTO_memcmp(expected_hash.data(), auth_data.data(), 32) == 0;
}

EVP_PKEY* es256_key_from_cose(std::span<const unsigned char> encoded) {
    CborReader reader(encoded);
    const auto count = reader.map_size();
    if (!count || *count != 5) {
        return nullptr;
    }
    std::map<long long, long long> integers;
    std::map<long long, std::vector<unsigned char>> byte_strings;
    for (std::size_t index = 0; index < *count; ++index) {
        const auto key = reader.integer();
        if (!key || reader.finished()) {
            return nullptr;
        }
        if (*key == -2 || *key == -3) {
            auto value = reader.byte_string();
            if (!value) {
                return nullptr;
            }
            byte_strings[*key] = std::move(*value);
        } else {
            const auto value = reader.integer();
            if (!value) {
                return nullptr;
            }
            integers[*key] = *value;
        }
    }
    if (!reader.finished() || integers[1] != 2 || integers[3] != -7 || integers[-1] != 1 ||
        byte_strings[-2].size() != 32 || byte_strings[-3].size() != 32) {
        return nullptr;
    }
    std::array<unsigned char, 65> public_key{};
    public_key[0] = 0x04;
    std::copy(byte_strings[-2].begin(), byte_strings[-2].end(), public_key.begin() + 1);
    std::copy(byte_strings[-3].begin(), byte_strings[-3].end(), public_key.begin() + 33);
    char group[] = "prime256v1";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(
            OSSL_PKEY_PARAM_PUB_KEY, public_key.data(), public_key.size()),
        OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* key = nullptr;
    if (context == nullptr || EVP_PKEY_fromdata_init(context) != 1 ||
        EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, parameters) != 1) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_CTX* check = key == nullptr ? nullptr : EVP_PKEY_CTX_new(key, nullptr);
    if (check == nullptr || EVP_PKEY_public_check(check) != 1) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    EVP_PKEY_CTX_free(check);
    return key;
}

std::string public_key_pem(EVP_PKEY* key) {
    BIO* output = BIO_new(BIO_s_mem());
    if (output == nullptr || PEM_write_bio_PUBKEY(output, key) != 1) {
        BIO_free(output);
        return {};
    }
    const char* data = nullptr;
    const long size = BIO_get_mem_data(output, &data);
    const std::string result = size > 0 ? std::string(data, size) : std::string();
    BIO_free(output);
    return result;
}

} // namespace

std::string random_secret() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    return hex(bytes.data(), bytes.size());
}

std::string hash_secret(std::string_view secret) {
    std::array<unsigned char, 16> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
        return {};
    EVP_KDF* algorithm = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!algorithm)
        return {};
    EVP_KDF_CTX* context = EVP_KDF_CTX_new(algorithm);
    EVP_KDF_free(algorithm);
    if (!context)
        return {};
    std::array<unsigned char, 32> derived{};
    unsigned int iterations = 3;
    unsigned int lanes = 1;
    unsigned int threads = 1;
    unsigned int memory_kib = 65536;
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string("pass", const_cast<char*>(secret.data()), secret.size()),
        OSSL_PARAM_construct_octet_string(
            "salt", const_cast<unsigned char*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_uint("iter", &iterations),
        OSSL_PARAM_construct_uint("lanes", &lanes),
        OSSL_PARAM_construct_uint("threads", &threads),
        OSSL_PARAM_construct_uint("memcost", &memory_kib),
        OSSL_PARAM_construct_end(),
    };
    const int result = EVP_KDF_derive(context, derived.data(), derived.size(), parameters);
    EVP_KDF_CTX_free(context);
    if (result != 1)
        return {};
    return "argon2id$65536$3$1$" + hex(salt.data(), salt.size()) + "$" +
           hex(derived.data(), derived.size());
}

bool verify_secret(std::string_view encoded, std::string_view secret) {
    std::array<std::string, 6> parts;
    std::size_t start = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t end = encoded.find('$', start);
        parts[index] = encoded.substr(start, end - start);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    if (parts[0] != "argon2id" || parts[1] != "65536" || parts[2] != "3" || parts[3] != "1")
        return false;
    const std::vector<unsigned char> salt = from_hex(parts[4]);
    const std::vector<unsigned char> expected = from_hex(parts[5]);
    if (salt.size() != 16 || expected.size() != 32)
        return false;
    EVP_KDF* algorithm = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!algorithm)
        return false;
    EVP_KDF_CTX* context = EVP_KDF_CTX_new(algorithm);
    EVP_KDF_free(algorithm);
    if (!context)
        return false;
    std::array<unsigned char, 32> derived{};
    unsigned int iterations = 3;
    unsigned int lanes = 1;
    unsigned int threads = 1;
    unsigned int memory_kib = 65536;
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string("pass", const_cast<char*>(secret.data()), secret.size()),
        OSSL_PARAM_construct_octet_string(
            "salt", const_cast<unsigned char*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_uint("iter", &iterations),
        OSSL_PARAM_construct_uint("lanes", &lanes),
        OSSL_PARAM_construct_uint("threads", &threads),
        OSSL_PARAM_construct_uint("memcost", &memory_kib),
        OSSL_PARAM_construct_end(),
    };
    const int result = EVP_KDF_derive(context, derived.data(), derived.size(), parameters);
    EVP_KDF_CTX_free(context);
    return result == 1 && CRYPTO_memcmp(derived.data(), expected.data(), derived.size()) == 0;
}

std::string new_totp_secret() {
    return random_secret();
}

bool verify_totp(std::string_view secret, std::string_view code) {
    if (code.size() != 6 || !std::all_of(code.begin(), code.end(), [](char value) {
            return value >= '0' && value <= '9';
        }))
        return false;
    const std::vector<unsigned char> key = from_hex(secret);
    if (key.empty())
        return false;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (long long offset = -1; offset <= 1; ++offset) {
        const std::uint64_t counter = static_cast<std::uint64_t>(now / 30 + offset);
        std::array<unsigned char, 8> message{};
        for (int index = 0; index < 8; ++index)
            message[7 - index] = static_cast<unsigned char>(counter >> (index * 8));
        unsigned int length = 0;
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        HMAC(EVP_sha1(),
             key.data(),
             static_cast<int>(key.size()),
             message.data(),
             message.size(),
             digest.data(),
             &length);
        const unsigned start = digest[length - 1] & 0x0f;
        const unsigned value = ((digest[start] & 0x7f) << 24) | (digest[start + 1] << 16) |
                               (digest[start + 2] << 8) | digest[start + 3];
        const std::string expected = std::format("{:06}", value % 1000000);
        if (CRYPTO_memcmp(expected.data(), code.data(), expected.size()) == 0)
            return true;
    }
    return false;
}

std::string sha256_hex(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    return hex(digest.data(), size);
}

std::string sha256_base64url(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    return base64url(digest.data(), size);
}

std::string base64url_encode(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::optional<OidcClaims> verify_oidc_id_token(std::string_view token,
                                               std::string_view jwks,
                                               std::string_view issuer,
                                               std::string_view client_id,
                                               std::string_view nonce,
                                               long now_unix) {
    const std::size_t first = token.find('.');
    const std::size_t second = first == std::string_view::npos ? first : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::vector<unsigned char> header_bytes = from_base64url(token.substr(0, first));
    const std::vector<unsigned char> claims_bytes =
        from_base64url(token.substr(first + 1, second - first - 1));
    const std::vector<unsigned char> signature = from_base64url(token.substr(second + 1));
    if (header_bytes.empty() || claims_bytes.empty() || signature.empty()) {
        return std::nullopt;
    }
    const nlohmann::json header = nlohmann::json::parse(header_bytes, nullptr, false);
    const nlohmann::json claims = nlohmann::json::parse(claims_bytes, nullptr, false);
    const nlohmann::json keys = nlohmann::json::parse(jwks, nullptr, false);
    if (header.is_discarded() || claims.is_discarded() || keys.is_discarded() ||
        header.value("alg", "") != "RS256" || !keys.contains("keys") || !keys["keys"].is_array()) {
        return std::nullopt;
    }
    const std::string key_id = header.value("kid", "");
    const auto key_entry =
        std::find_if(keys["keys"].begin(), keys["keys"].end(), [&](const nlohmann::json& entry) {
            return entry.value("kid", "") == key_id && entry.value("kty", "") == "RSA" &&
                   (entry.value("alg", "RS256") == "RS256") && (entry.value("use", "sig") == "sig");
        });
    if (key_entry == keys["keys"].end()) {
        return std::nullopt;
    }
    EVP_PKEY* key = rsa_key_from_jwk(*key_entry);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const std::string_view signed_input = token.substr(0, second);
    const int verified =
        key != nullptr && context != nullptr &&
        EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(context, signed_input.data(), signed_input.size()) == 1 &&
        EVP_DigestVerifyFinal(context, signature.data(), signature.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!verified) {
        return std::nullopt;
    }
    if (now_unix == 0) {
        now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    }
    const long expires_at = claims.value("exp", 0L);
    const long not_before = claims.value("nbf", 0L);
    const long issued_at = claims.value("iat", 0L);
    const std::string subject = claims.value("sub", "");
    const nlohmann::json audience = claims.value("aud", nlohmann::json());
    const bool authorized_party_valid =
        !audience.is_array() || audience.size() <= 1 || claims.value("azp", "") == client_id;
    if (claims.value("iss", "") != issuer || !audience_contains(audience, client_id) ||
        !authorized_party_valid || expires_at <= now_unix || not_before > now_unix + 60 ||
        issued_at > now_unix + 60 || subject.empty() || claims.value("nonce", "") != nonce) {
        return std::nullopt;
    }
    return OidcClaims{subject, claims.value("email", ""), claims.value("name", "")};
}

std::expected<VerifiedPasskeyRegistration, std::string>
verify_passkey_registration(std::string_view client_data_json,
                            std::string_view attestation_object,
                            std::string_view challenge,
                            std::string_view rp_id,
                            std::string_view origin) {
    const auto client_data =
        verified_client_data(client_data_json, "webauthn.create", challenge, origin);
    if (!client_data) {
        return std::unexpected(client_data.error());
    }
    const std::vector<unsigned char> attestation_bytes = from_base64url(attestation_object);
    if (attestation_bytes.empty()) {
        return std::unexpected("WebAuthn attestation object is not valid base64url");
    }
    const nlohmann::json attestation = nlohmann::json::from_cbor(attestation_bytes, true, false);
    if (attestation.is_discarded() || !attestation.is_object() || !attestation.contains("fmt") ||
        !attestation["fmt"].is_string() || attestation["fmt"].get<std::string>() != "none" ||
        !attestation.contains("attStmt") || !attestation["attStmt"].is_object() ||
        !attestation["attStmt"].empty() || !attestation.contains("authData") ||
        !attestation["authData"].is_binary()) {
        return std::unexpected("WebAuthn registration requires attestation format none");
    }
    const auto& auth_binary = attestation["authData"].get_binary();
    const std::span<const unsigned char> auth_data(auth_binary.data(), auth_binary.size());
    if (!verified_authenticator_scope(auth_data, rp_id) || (auth_data[32] & 0x40) == 0 ||
        (auth_data[32] & 0x80) != 0 || auth_data.size() < 55) {
        return std::unexpected("WebAuthn registration authenticator data is invalid");
    }
    const std::size_t credential_size =
        (static_cast<std::size_t>(auth_data[53]) << 8) | auth_data[54];
    if (credential_size == 0 || credential_size > 1023 || credential_size > auth_data.size() - 55) {
        return std::unexpected("WebAuthn credential id is invalid");
    }
    const std::span<const unsigned char> credential = auth_data.subspan(55, credential_size);
    const std::span<const unsigned char> cose_key = auth_data.subspan(55 + credential_size);
    EVP_PKEY* key = es256_key_from_cose(cose_key);
    if (key == nullptr) {
        return std::unexpected("WebAuthn credential is not an ES256 public key");
    }
    const std::string pem = public_key_pem(key);
    EVP_PKEY_free(key);
    if (pem.empty()) {
        return std::unexpected("WebAuthn public key could not be stored");
    }
    return VerifiedPasskeyRegistration{base64url(credential.data(), credential.size()),
                                       pem,
                                       network_uint32(auth_data.subspan(33, 4))};
}

std::expected<std::uint32_t, std::string>
verify_passkey_assertion(std::string_view client_data_json,
                         std::string_view authenticator_data,
                         std::string_view signature,
                         std::string_view public_key_pem,
                         std::string_view challenge,
                         std::string_view rp_id,
                         std::string_view origin) {
    const auto client_data =
        verified_client_data(client_data_json, "webauthn.get", challenge, origin);
    if (!client_data) {
        return std::unexpected(client_data.error());
    }
    const std::vector<unsigned char> auth_data = from_base64url(authenticator_data);
    const std::vector<unsigned char> signature_bytes = from_base64url(signature);
    if (!verified_authenticator_scope(auth_data, rp_id) || auth_data.size() != 37 ||
        signature_bytes.empty()) {
        return std::unexpected("WebAuthn assertion authenticator data is invalid");
    }
    BIO* input = BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size()));
    EVP_PKEY* key =
        input == nullptr ? nullptr : PEM_read_bio_PUBKEY(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    if (key == nullptr || EVP_PKEY_is_a(key, "EC") != 1) {
        EVP_PKEY_free(key);
        return std::unexpected("stored passkey public key is invalid");
    }
    const std::vector<unsigned char> client_hash = sha256(*client_data);
    std::vector<unsigned char> signed_data = auth_data;
    signed_data.insert(signed_data.end(), client_hash.begin(), client_hash.end());
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const int verified =
        context != nullptr &&
        EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(context, signed_data.data(), signed_data.size()) == 1 &&
        EVP_DigestVerifyFinal(context, signature_bytes.data(), signature_bytes.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!verified) {
        return std::unexpected("WebAuthn assertion signature is invalid");
    }
    return network_uint32(std::span(auth_data).subspan(33, 4));
}

} // namespace identity_crypto

namespace {

std::optional<std::string> nullable(const std::string& s) {
    if (s.empty())
        return std::nullopt;
    return s;
}

std::string hex_bytes(const unsigned char* bytes, std::size_t count) {
    std::string result;
    result.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        result += std::format("{:02x}", static_cast<unsigned>(bytes[index]));
    }
    return result;
}

std::optional<std::vector<unsigned char>> decode_hex(std::string_view value) {
    if (value.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        unsigned parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data() + index, value.data() + index + 2, parsed, 16);
        if (error != std::errc{} || end != value.data() + index + 2) {
            return std::nullopt;
        }
        result.push_back(static_cast<unsigned char>(parsed));
    }
    return result;
}

std::optional<std::vector<std::byte>> decode_base64url(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string padded(value);
    std::replace(padded.begin(), padded.end(), '-', '+');
    std::replace(padded.begin(), padded.end(), '_', '/');
    while (padded.size() % 4 != 0) {
        padded.push_back('=');
    }
    std::vector<unsigned char> decoded((padded.size() / 4) * 3);
    const int count = EVP_DecodeBlock(decoded.data(),
                                      reinterpret_cast<const unsigned char*>(padded.data()),
                                      static_cast<int>(padded.size()));
    if (count < 0) {
        return std::nullopt;
    }
    std::size_t padding = 0;
    if (padded.ends_with('=')) {
        ++padding;
    }
    if (padded.size() > 1 && padded[padded.size() - 2] == '=') {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(count) - padding);
    std::vector<std::byte> result;
    result.reserve(decoded.size());
    for (const unsigned char byte : decoded) {
        result.push_back(static_cast<std::byte>(byte));
    }
    return result;
}

struct VerifiedIdentityDocument {
    std::string identifier;
    std::string document;
    std::string hash;
};

constexpr const char* user_columns =
    "id::text, coalesce(taap_id,''), coalesce(email,''), display_name, "
    "created_at::text, updated_at::text";
constexpr const char* organization_columns =
    "id::text, coalesce(taap_id,''), display_name, created_at::text, "
    "updated_at::text";
constexpr const char* customer_columns = "id::text, customer_type, coalesce(user_id::text,''), "
                                         "coalesce(organization_id::text,''), name, "
                                         "created_at::text, updated_at::text";

constexpr const char* repository_columns =
    "id::text, customer_id::text, name, description, visibility, "
    "coalesce(deleted_at::text,''), coalesce(retention_until::text,''), "
    "created_at::text, updated_at::text";
constexpr const char* team_columns =
    "id::text, organization_id::text, name, display_name, created_at::text, "
    "updated_at::text";
constexpr const char* membership_columns =
    "id::text, organization_id::text, coalesce(team_id::text,''), "
    "principal_type, principal_id, "
    "principal_taap_id, role, attestation_digest, granted_by_taap_id, "
    "created_at::text, "
    "updated_at::text";
constexpr const char* invitation_columns =
    "id::text, organization_id::text, coalesce(team_id::text,''), "
    "invitee_type, invitee, role, "
    "expires_at::text, status, coalesce(accepted_membership_id::text,''), "
    "created_by_taap_id, "
    "created_at::text, updated_at::text";
constexpr const char* object_columns =
    "id::text, object_type, external_id, coalesce(parent_id::text,''), "
    "inherit, created_at::text, "
    "updated_at::text";
constexpr const char* grant_columns =
    "id::text, principal_taap_id, object_id::text, role, attestation_digest, "
    "granted_by_taap_id, "
    "created_at::text, updated_at::text";
constexpr const char* identity_columns =
    "id::text, taap_id, public_key, wrapped_seed, created_at::text, "
    "coalesce(rotation_document::text,''), current";
constexpr const char* audit_event_columns =
    "id::text, to_char(occurred_at AT TIME ZONE 'UTC', "
    "'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
    "coalesce(customer_id::text,''), kind, principal, object, action, outcome, "
    "request_id::text, "
    "coalesce(method,''), coalesce(device,''), details::text, "
    "coalesce(previous_digest,''), "
    "signer, signature, digest";

Repository repository_of(const pqxx::row& r) {
    return Repository{r[0].as<std::string>(),
                      r[1].as<std::string>(),
                      r[2].as<std::string>(),
                      r[3].as<std::string>(),
                      r[4].as<std::string>(),
                      r[5].as<std::string>(),
                      r[6].as<std::string>(),
                      r[7].as<std::string>(),
                      r[8].as<std::string>()};
}

RepositoryRef repository_ref_of(const pqxx::row& row) {
    return RepositoryRef{row[0].as<std::string>(),
                         row[1].as<std::string>(),
                         row[2].as<std::string>(),
                         row[3].as<std::string>()};
}

Result<void> require_live_repository(pqxx::work& tx, const std::string& repository_id) {
    const pqxx::result rows = tx.exec_params(
        "SELECT deleted_at FROM repositories WHERE id = $1::uuid FOR UPDATE", repository_id);
    if (rows.empty()) {
        return std::unexpected(StoreError{"not_found", "no repository " + repository_id});
    }
    if (!rows[0][0].is_null()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    return {};
}

Team team_of(const pqxx::row& row) {
    return Team{row[0].as<std::string>(),
                row[1].as<std::string>(),
                row[2].as<std::string>(),
                row[3].as<std::string>(),
                row[4].as<std::string>(),
                row[5].as<std::string>()};
}

Membership membership_of(const pqxx::row& row) {
    return Membership{row[0].as<std::string>(),
                      row[1].as<std::string>(),
                      row[2].as<std::string>(),
                      row[3].as<std::string>(),
                      row[4].as<std::string>(),
                      row[5].as<std::string>(),
                      row[6].as<std::string>(),
                      row[7].as<std::string>(),
                      row[8].as<std::string>(),
                      row[9].as<std::string>(),
                      row[10].as<std::string>()};
}

Invitation invitation_of(const pqxx::row& row) {
    return Invitation{row[0].as<std::string>(),
                      row[1].as<std::string>(),
                      row[2].as<std::string>(),
                      row[3].as<std::string>(),
                      row[4].as<std::string>(),
                      row[5].as<std::string>(),
                      row[6].as<std::string>(),
                      row[7].as<std::string>(),
                      row[8].as<std::string>(),
                      row[9].as<std::string>(),
                      row[10].as<std::string>(),
                      row[11].as<std::string>()};
}

PolicyObject object_of(const pqxx::row& row) {
    return PolicyObject{row[0].as<std::string>(),
                        row[1].as<std::string>(),
                        row[2].as<std::string>(),
                        row[3].as<std::string>(),
                        row[5].as<std::string>(),
                        row[6].as<std::string>(),
                        row[4].as<bool>()};
}

Grant grant_of(const pqxx::row& row) {
    return Grant{row[0].as<std::string>(),
                 row[1].as<std::string>(),
                 row[2].as<std::string>(),
                 row[3].as<std::string>(),
                 row[4].as<std::string>(),
                 row[5].as<std::string>(),
                 row[6].as<std::string>(),
                 row[7].as<std::string>()};
}

std::string uuid_array(const std::vector<std::string>& values) {
    std::string out = "{";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out += ',';
        }
        out += values[index];
    }
    out += '}';
    return out;
}

User user_of(const pqxx::row& r) {
    return User{r[0].as<std::string>(),
                r[1].as<std::string>(),
                r[2].as<std::string>(),
                r[3].as<std::string>(),
                r[4].as<std::string>(),
                r[5].as<std::string>()};
}
Organization organization_of(const pqxx::row& r) {
    return Organization{r[0].as<std::string>(),
                        r[1].as<std::string>(),
                        r[2].as<std::string>(),
                        r[3].as<std::string>(),
                        r[4].as<std::string>()};
}
Customer customer_of(const pqxx::row& r) {
    return Customer{r[0].as<std::string>(),
                    r[1].as<std::string>(),
                    r[2].as<std::string>(),
                    r[3].as<std::string>(),
                    r[4].as<std::string>(),
                    r[5].as<std::string>(),
                    r[6].as<std::string>()};
}

audit::Identity identity_of(const pqxx::row& r) {
    return audit::Identity{
        .id = r[0].as<std::string>(),
        .taap_id = r[1].as<std::string>(),
        .public_key = r[2].as<std::string>(),
        .wrapped_seed = r[3].as<std::string>(),
        .created_at = r[4].as<std::string>(),
        .rotation_document = r[5].as<std::string>(),
        .current = r[6].as<bool>(),
    };
}

audit::Event audit_event_of(const pqxx::row& r) {
    return audit::Event{
        .id = r[0].as<std::string>(),
        .occurred_at = r[1].as<std::string>(),
        .draft =
            audit::EventDraft{
                .customer_id = r[2].as<std::string>(),
                .kind = r[3].as<std::string>(),
                .principal = r[4].as<std::string>(),
                .object = r[5].as<std::string>(),
                .action = r[6].as<std::string>(),
                .outcome = r[7].as<std::string>(),
                .request_id = r[8].as<std::string>(),
                .method = r[9].as<std::string>(),
                .device = r[10].as<std::string>(),
                .details = nlohmann::json::parse(r[11].as<std::string>()),
            },
        .previous_digest = r[12].as<std::string>(),
        .signer = r[13].as<std::string>(),
        .signature = r[14].as<std::string>(),
        .digest = r[15].as<std::string>(),
    };
}
Device device_of(const pqxx::row& r) {
    return Device{r[0].as<std::string>(),
                  r[1].as<std::string>(),
                  r[2].as<std::string>(),
                  r[3].as<std::string>(),
                  r[4].as<std::string>(),
                  r[5].as<std::string>()};
}

Session session_of(const pqxx::row& r) {
    return Session{r[0].as<std::string>(),
                   r[1].as<std::string>(),
                   r[2].as<std::string>(),
                   r[3].as<std::string>(),
                   r[4].as<std::string>(),
                   r[5].as<std::string>(),
                   r[6].as<std::string>(),
                   r[7].as<std::string>(),
                   r[8].as<std::string>(),
                   r[9].as<std::string>(),
                   r[10].as<std::string>(),
                   nlohmann::json::parse(r[11].as<std::string>()).get<std::vector<std::string>>()};
}

constexpr const char* device_columns =
    "id::text, user_id::text, name, coalesce(revoked_at::text,''), "
    "created_at::text, updated_at::text";
constexpr const char* session_columns =
    "id::text, coalesce(user_id::text,''), coalesce(device_id::text,''), "
    "pool_id, kind, name, reason, coalesce(expires_at::text,''), "
    "coalesce(revoked_at::text,''), created_at::text, "
    "coalesce(last_used_at::text,''), array_to_json(scopes)::text";
constexpr const char* qualified_session_columns =
    "sessions.id::text, coalesce(sessions.user_id::text,''), "
    "coalesce(sessions.device_id::text,''), sessions.pool_id, sessions.kind, "
    "sessions.name, sessions.reason, coalesce(sessions.expires_at::text,''), "
    "coalesce(sessions.revoked_at::text,''), sessions.created_at::text, "
    "coalesce(sessions.last_used_at::text,''), "
    "array_to_json(sessions.scopes)::text";

class PostgresStore final : public Store {
public:
    explicit PostgresStore(std::string uri) : uri_(std::move(uri)) {}
    std::string kind() const override {
        return "postgres";
    }

    Result<void> ping() override {
        Result<void> result = with_tx<void>([](pqxx::work& tx) -> Result<void> {
            tx.exec("SET LOCAL statement_timeout = '1000ms'");
            tx.exec("SELECT 1");
            return {};
        });
        if (!result)
            return std::unexpected(StoreError{"store_unavailable", result.error().message});
        return {};
    }

    Result<User> create_user(const NewUser& in) override {
        return with_tx<User>([&](pqxx::work& tx) -> Result<User> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO users (taap_id, email, display_name) VALUES "
                            "($1, $2, $3) RETURNING ") +
                    user_columns,
                nullable(in.taap_id),
                nullable(in.email),
                in.display_name);
            return user_of(rows[0]);
        });
    }
    Result<std::vector<User>> list_users(int limit) override {
        return with_tx<std::vector<User>>([&](pqxx::work& tx) -> Result<std::vector<User>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + user_columns +
                                   " FROM users ORDER BY created_at DESC LIMIT $1",
                               limit);
            std::vector<User> out;
            for (const auto& r : rows)
                out.push_back(user_of(r));
            return out;
        });
    }
    Result<User> get_user(const std::string& id) override {
        return with_tx<User>([&](pqxx::work& tx) -> Result<User> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + user_columns + " FROM users WHERE id = $1::uuid", id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no user " + id});
            return user_of(rows[0]);
        });
    }

    Result<Organization> create_organization(const NewOrganization& in) override {
        return with_tx<Organization>([&](pqxx::work& tx) -> Result<Organization> {
            std::optional<std::string> parent_id;
            if (!in.customer_id.empty()) {
                const pqxx::result parents =
                    tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                                   "'customer' AND external_id = $1",
                                   in.customer_id);
                if (parents.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "customer_id does not reference a policy object"});
                }
                parent_id = parents[0][0].as<std::string>();
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO organizations (taap_id, display_name) "
                                           "VALUES ($1, $2) RETURNING ") +
                                   organization_columns,
                               nullable(in.taap_id),
                               in.display_name);
            Organization organization = organization_of(rows[0]);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('organization', $1, $2::uuid)",
                           organization.id,
                           parent_id);
            return organization;
        });
    }
    Result<std::vector<Organization>> list_organizations(int limit) override {
        return with_tx<std::vector<Organization>>(
            [&](pqxx::work& tx) -> Result<std::vector<Organization>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + organization_columns +
                                       " FROM organizations ORDER BY created_at DESC LIMIT $1",
                                   limit);
                std::vector<Organization> out;
                for (const auto& r : rows)
                    out.push_back(organization_of(r));
                return out;
            });
    }
    Result<Organization> get_organization(const std::string& id) override {
        return with_tx<Organization>([&](pqxx::work& tx) -> Result<Organization> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + organization_columns +
                                                         " FROM organizations WHERE id = $1::uuid",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no organization " + id});
            return organization_of(rows[0]);
        });
    }

    Result<Customer> create_customer(const NewCustomer& in) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO customers (customer_type, user_id, "
                                           "organization_id, name) "
                                           "VALUES ($1, $2::uuid, $3::uuid, $4) RETURNING ") +
                                   customer_columns,
                               in.customer_type,
                               nullable(in.user_id),
                               nullable(in.organization_id),
                               in.name);
            Customer customer = customer_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id) "
                               "VALUES ('customer', $1) "
                               "RETURNING id::text",
                               customer.id);
            if (!customer.organization_id.empty()) {
                tx.exec_params("UPDATE objects SET parent_id = $1::uuid "
                               "WHERE object_type = 'organization' AND external_id = $2",
                               objects[0][0].as<std::string>(),
                               customer.organization_id);
            }
            return customer;
        });
    }
    Result<std::vector<Customer>> list_customers(int limit) override {
        return with_tx<std::vector<Customer>>([&](pqxx::work& tx) -> Result<std::vector<Customer>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + customer_columns +
                                   " FROM customers ORDER BY created_at DESC LIMIT $1",
                               limit);
            std::vector<Customer> out;
            for (const auto& r : rows)
                out.push_back(customer_of(r));
            return out;
        });
    }
    Result<Customer> get_customer(const std::string& id) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + customer_columns +
                                                         " FROM customers WHERE id = $1::uuid",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no customer " + id});
            return customer_of(rows[0]);
        });
    }

    Result<Repository> create_repository(const NewRepository& in) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                               "'customer' AND external_id = $1",
                               in.customer_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "customer_id does not reference a policy object"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO repositories (customer_id, name, "
                                           "description, visibility) "
                                           "VALUES ($1::uuid, $2, $3, $4) RETURNING ") +
                                   repository_columns,
                               in.customer_id,
                               in.name,
                               in.description,
                               in.visibility.empty() ? std::string("private") : in.visibility);
            Repository repository = repository_of(rows[0]);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('repository', $1, $2::uuid)",
                           repository.id,
                           parents[0][0].as<std::string>());
            return repository;
        });
    }
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                      int limit) override {
        return with_tx<std::vector<Repository>>(
            [&](pqxx::work& tx) -> Result<std::vector<Repository>> {
                const pqxx::result rows =
                    customer_id.empty()
                        ? tx.exec_params(std::string("SELECT ") + repository_columns +
                                             " FROM repositories ORDER BY created_at DESC LIMIT $1",
                                         limit)
                        : tx.exec_params(std::string("SELECT ") + repository_columns +
                                             " FROM repositories WHERE customer_id = "
                                             "$1::uuid ORDER BY created_at DESC LIMIT $2",
                                         customer_id,
                                         limit);
                std::vector<Repository> out;
                for (const auto& r : rows)
                    out.push_back(repository_of(r));
                return out;
            });
    }
    Result<Repository> get_repository(const std::string& id) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + repository_columns +
                                                         " FROM repositories WHERE id = $1::uuid",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no repository " + id});
            return repository_of(rows[0]);
        });
    }

    Result<Repository> rename_repository(const std::string& id, const std::string& name) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            if (const auto live = require_live_repository(tx, id); !live) {
                return std::unexpected(live.error());
            }
            const pqxx::result rows =
                tx.exec_params(std::string("UPDATE repositories SET name = $2 WHERE "
                                           "id = $1::uuid RETURNING ") +
                                   repository_columns,
                               id,
                               name);
            return repository_of(rows[0]);
        });
    }

    Result<Repository> transfer_repository(const std::string& id,
                                           const std::string& customer_id) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            if (const auto live = require_live_repository(tx, id); !live) {
                return std::unexpected(live.error());
            }
            const pqxx::result customer =
                tx.exec_params("SELECT id FROM customers WHERE id = $1::uuid", customer_id);
            if (customer.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "customer_id does not reference a customer"});
            }
            const pqxx::result parent =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                               "'customer' AND external_id = $1",
                               customer_id);
            if (parent.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "customer policy object is missing"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("UPDATE repositories SET customer_id = "
                                           "$2::uuid WHERE id = $1::uuid "
                                           "RETURNING ") +
                                   repository_columns,
                               id,
                               customer_id);
            tx.exec_params("UPDATE objects SET parent_id = $1::uuid WHERE "
                           "object_type = 'repository' "
                           "AND external_id = $2",
                           parent[0][0].as<std::string>(),
                           id);
            return repository_of(rows[0]);
        });
    }

    Result<Repository> set_repository_visibility(const std::string& id,
                                                 const std::string& visibility) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            if (visibility != "private" && visibility != "organization" && visibility != "public") {
                return std::unexpected(StoreError{
                    "invalid_input", "visibility must be private, organization, or public"});
            }
            if (const auto live = require_live_repository(tx, id); !live) {
                return std::unexpected(live.error());
            }
            const pqxx::result rows =
                tx.exec_params(std::string("UPDATE repositories SET visibility = $2 "
                                           "WHERE id = $1::uuid RETURNING ") +
                                   repository_columns,
                               id,
                               visibility);
            return repository_of(rows[0]);
        });
    }

    Result<Repository> delete_repository(const std::string& id,
                                         const RepositoryDeletion& deletion) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            if (const auto live = require_live_repository(tx, id); !live) {
                return std::unexpected(live.error());
            }
            const pqxx::result rows =
                tx.exec_params(std::string("UPDATE repositories SET deleted_at = $2::timestamptz, "
                                           "retention_until = $3::timestamptz WHERE id = $1::uuid "
                                           "RETURNING ") +
                                   repository_columns,
                               id,
                               deletion.deleted_at,
                               deletion.retention_until);
            return repository_of(rows[0]);
        });
    }

    Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) override {
        return with_tx<std::vector<RepositoryRef>>(
            [&](pqxx::work& tx) -> Result<std::vector<RepositoryRef>> {
                const pqxx::result rows =
                    tx.exec_params("SELECT repository_id::text, name, head, "
                                   "updated_at::text FROM repository_refs "
                                   "WHERE repository_id = $1::uuid ORDER BY name",
                                   repository_id);
                std::vector<RepositoryRef> references;
                for (const auto& row : rows) {
                    references.push_back(repository_ref_of(row));
                }
                return references;
            });
    }

    Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                             const std::string& name,
                                             const std::string& head) override {
        return with_tx<RepositoryRef>([&](pqxx::work& tx) -> Result<RepositoryRef> {
            const pqxx::result rows =
                tx.exec_params("INSERT INTO repository_refs (repository_id, name, head) VALUES "
                               "($1::uuid, $2, $3) ON CONFLICT (repository_id, name) DO UPDATE SET "
                               "head = EXCLUDED.head, updated_at = now() RETURNING "
                               "repository_id::text, "
                               "name, head, updated_at::text",
                               repository_id,
                               name,
                               head);
            return repository_ref_of(rows[0]);
        });
    }

    Result<Team> create_team(const NewTeam& in) override {
        return with_tx<Team>([&](pqxx::work& tx) -> Result<Team> {
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects "
                               "WHERE object_type = 'organization' AND external_id = $1",
                               in.organization_id);
            if (parents.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "organization_id does not reference a policy object"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO teams (organization_id, name, display_name) "
                            "VALUES ($1::uuid, $2, $3) RETURNING ") +
                    team_columns,
                in.organization_id,
                in.name,
                in.display_name);
            Team team = team_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('team', $1, $2::uuid) RETURNING id::text",
                               team.id,
                               parents[0][0].as<std::string>());
            return team;
        });
    }

    Result<std::vector<Team>> list_teams(const std::string& organization_id, int limit) override {
        return with_tx<std::vector<Team>>([&](pqxx::work& tx) -> Result<std::vector<Team>> {
            const pqxx::result rows =
                organization_id.empty()
                    ? tx.exec_params(std::string("SELECT ") + team_columns +
                                         " FROM teams ORDER BY created_at DESC LIMIT $1",
                                     limit)
                    : tx.exec_params(std::string("SELECT ") + team_columns +
                                         " FROM teams WHERE organization_id = $1::uuid "
                                         "ORDER BY created_at DESC LIMIT $2",
                                     organization_id,
                                     limit);
            std::vector<Team> out;
            for (const auto& row : rows) {
                out.push_back(team_of(row));
            }
            return out;
        });
    }

    Result<Team> get_team(const std::string& id) override {
        return with_tx<Team>([&](pqxx::work& tx) -> Result<Team> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + team_columns + " FROM teams WHERE id = $1::uuid", id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no team " + id});
            }
            return team_of(rows[0]);
        });
    }

    Result<Membership> create_membership(const NewMembership& in) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            if (in.principal_type == "user") {
                const pqxx::result users =
                    tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
                                   in.principal_id,
                                   in.principal_taap_id);
                if (users.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "user principal does not match its TAAP identity"});
                }
            }
            const std::string parent_type = in.team_id.empty() ? "organization" : "team";
            const std::string parent_external_id =
                in.team_id.empty() ? in.organization_id : in.team_id;
            if (!in.team_id.empty()) {
                const pqxx::result teams =
                    tx.exec_params("SELECT 1 FROM teams WHERE id = $1::uuid AND "
                                   "organization_id = $2::uuid",
                                   in.team_id,
                                   in.organization_id);
                if (teams.empty()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "team_id does not belong to the organization"});
                }
            }
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                               "AND external_id = $2",
                               parent_type,
                               parent_external_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "membership policy parent is missing"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO memberships (organization_id, team_id, "
                            "principal_type, "
                            "principal_id, principal_taap_id, role, attestation_digest, "
                            "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
                            "$7, "
                            "$8) RETURNING ") +
                    membership_columns,
                in.organization_id,
                nullable(in.team_id),
                in.principal_type,
                in.principal_id,
                in.principal_taap_id,
                in.role,
                in.attestation_digest,
                in.granted_by_taap_id);
            Membership membership = membership_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('member', $1, $2::uuid) RETURNING id::text",
                               membership.id,
                               parents[0][0].as<std::string>());
            const std::string grant_role = in.role == "member" ? "read" : "admin";
            tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                           "attestation_digest, "
                           "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                           in.principal_taap_id,
                           parents[0][0].as<std::string>(),
                           grant_role,
                           in.attestation_digest,
                           in.granted_by_taap_id);
            return membership;
        });
    }

    Result<std::vector<Membership>> list_memberships(const std::string& organization_id,
                                                     const std::string& team_id,
                                                     int limit) override {
        return with_tx<std::vector<Membership>>(
            [&](pqxx::work& tx) -> Result<std::vector<Membership>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!organization_id.empty()) {
                    params.append(organization_id);
                    where += " AND organization_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!team_id.empty()) {
                    params.append(team_id);
                    where += " AND team_id = $" + std::to_string(params.size()) + "::uuid";
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + membership_columns + " FROM memberships" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
                const pqxx::result rows = tx.exec(query, params);
                std::vector<Membership> out;
                for (const auto& row : rows) {
                    out.push_back(membership_of(row));
                }
                return out;
            });
    }

    Result<Invitation> create_invitation(const NewInvitation& in) override {
        return with_tx<Invitation>([&](pqxx::work& tx) -> Result<Invitation> {
            if (!in.team_id.empty()) {
                const pqxx::result teams =
                    tx.exec_params("SELECT 1 FROM teams WHERE id = $1::uuid AND "
                                   "organization_id = $2::uuid",
                                   in.team_id,
                                   in.organization_id);
                if (teams.empty()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "team_id does not belong to the organization"});
                }
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO invitations (organization_id, team_id, "
                                           "invitee_type, "
                                           "invitee, role, expires_at, created_by_taap_id) "
                                           "VALUES ($1::uuid, $2::uuid, $3, lower($4), $5, "
                                           "$6::timestamptz, $7) "
                                           "RETURNING ") +
                                   invitation_columns,
                               in.organization_id,
                               nullable(in.team_id),
                               in.invitee_type,
                               in.invitee,
                               in.role,
                               in.expires_at,
                               in.created_by_taap_id);
            Invitation invitation = invitation_of(rows[0]);
            const std::string target_type = in.team_id.empty() ? "organization" : "team";
            const std::string target_external_id =
                in.team_id.empty() ? in.organization_id : in.team_id;
            const pqxx::result objects =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                               "AND external_id = $2",
                               target_type,
                               target_external_id);
            return invitation;
        });
    }

    Result<std::vector<Invitation>> list_invitations(const std::string& organization_id,
                                                     const std::string& team_id,
                                                     int limit) override {
        return with_tx<std::vector<Invitation>>(
            [&](pqxx::work& tx) -> Result<std::vector<Invitation>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!organization_id.empty()) {
                    params.append(organization_id);
                    where += " AND organization_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!team_id.empty()) {
                    params.append(team_id);
                    where += " AND team_id = $" + std::to_string(params.size()) + "::uuid";
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + invitation_columns + " FROM invitations" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
                const pqxx::result rows = tx.exec(query, params);
                std::vector<Invitation> out;
                for (const auto& row : rows) {
                    out.push_back(invitation_of(row));
                }
                return out;
            });
    }

    Result<Invitation> get_invitation(const std::string& id) override {
        return with_tx<Invitation>([&](pqxx::work& tx) -> Result<Invitation> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + invitation_columns +
                                                         " FROM invitations WHERE id = $1::uuid",
                                                     id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no invitation " + id});
            }
            return invitation_of(rows[0]);
        });
    }

    Result<Membership> accept_invitation(const AcceptInvitation& in) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            const pqxx::result invitations =
                tx.exec_params(std::string("SELECT ") + invitation_columns +
                                   " FROM invitations WHERE id = $1::uuid FOR UPDATE",
                               in.invitation_id);
            if (invitations.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no invitation " + in.invitation_id});
            }
            Invitation invitation = invitation_of(invitations[0]);
            if (invitation.status != "pending") {
                return std::unexpected(StoreError{"conflict", "invitation is not pending"});
            }
            if (tx.exec_params("SELECT $1::timestamptz <= now()", invitation.expires_at)[0][0]
                    .as<bool>()) {
                return std::unexpected(StoreError{"conflict", "invitation has expired"});
            }
            if (in.principal_type == "user") {
                const pqxx::result users =
                    tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
                                   in.principal_id,
                                   in.principal_taap_id);
                if (users.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "user principal does not match its TAAP identity"});
                }
            }
            const std::string parent_type = invitation.team_id.empty() ? "organization" : "team";
            const std::string parent_external_id =
                invitation.team_id.empty() ? invitation.organization_id : invitation.team_id;
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                               "AND external_id = $2",
                               parent_type,
                               parent_external_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "membership policy parent is missing"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO memberships (organization_id, team_id, "
                            "principal_type, "
                            "principal_id, principal_taap_id, role, attestation_digest, "
                            "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
                            "$7, "
                            "$8) RETURNING ") +
                    membership_columns,
                invitation.organization_id,
                nullable(invitation.team_id),
                in.principal_type,
                in.principal_id,
                in.principal_taap_id,
                invitation.role,
                in.attestation_digest,
                in.accepted_by_taap_id);
            Membership membership = membership_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('member', $1, $2::uuid) RETURNING id::text",
                               membership.id,
                               parents[0][0].as<std::string>());
            const std::string grant_role = invitation.role == "member" ? "read" : "admin";
            tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                           "attestation_digest, "
                           "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                           in.principal_taap_id,
                           parents[0][0].as<std::string>(),
                           grant_role,
                           in.attestation_digest,
                           in.accepted_by_taap_id);
            tx.exec_params("UPDATE invitations SET status = 'accepted', "
                           "accepted_membership_id = $1::uuid "
                           "WHERE id = $2::uuid",
                           membership.id,
                           invitation.id);
            return membership;
        });
    }

    Result<PolicyObject> create_policy_object(const NewPolicyObject& in) override {
        return with_tx<PolicyObject>([&](pqxx::work& tx) -> Result<PolicyObject> {
            if (in.parent_id.empty() && in.object_type != "customer") {
                return std::unexpected(StoreError{
                    "invalid_input", "only a customer policy object may have no parent"});
            }
            if (!in.parent_id.empty()) {
                const pqxx::result parents = tx.exec_params(
                    "SELECT object_type FROM objects WHERE id = $1::uuid", in.parent_id);
                if (parents.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "parent_id does not reference a policy object"});
                }
                if (!object_parent_allowed(in.object_type, parents[0][0].as<std::string>())) {
                    return std::unexpected(
                        StoreError{"invalid_input", "object type is not valid under this parent"});
                }
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO objects (object_type, external_id, "
                                           "parent_id, inherit) "
                                           "VALUES ($1, $2, $3::uuid, $4) RETURNING ") +
                                   object_columns,
                               in.object_type,
                               in.external_id,
                               nullable(in.parent_id),
                               in.inherit);
            PolicyObject object = object_of(rows[0]);
            return object;
        });
    }

    Result<PolicyObject> get_policy_object(const std::string& id) override {
        return with_tx<PolicyObject>([&](pqxx::work& tx) -> Result<PolicyObject> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + object_columns + " FROM objects WHERE id = $1::uuid", id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no policy object " + id});
            }
            return object_of(rows[0]);
        });
    }

    Result<PolicyObject> find_policy_object(const std::string& object_type,
                                            const std::string& external_id) override {
        return with_tx<PolicyObject>([&](pqxx::work& tx) -> Result<PolicyObject> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + object_columns +
                                   " FROM objects WHERE object_type = $1 AND external_id = $2",
                               object_type,
                               external_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no " + object_type + " object " + external_id});
            }
            return object_of(rows[0]);
        });
    }

    Result<std::vector<PolicyObject>> list_policy_objects(const std::string& parent_id,
                                                          const std::string& object_type,
                                                          int limit) override {
        return with_tx<std::vector<PolicyObject>>(
            [&](pqxx::work& tx) -> Result<std::vector<PolicyObject>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!parent_id.empty()) {
                    params.append(parent_id);
                    where += " AND parent_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!object_type.empty()) {
                    params.append(object_type);
                    where += " AND object_type = $" + std::to_string(params.size());
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + object_columns + " FROM objects" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
                const pqxx::result rows = tx.exec(query, params);
                std::vector<PolicyObject> out;
                for (const auto& row : rows) {
                    out.push_back(object_of(row));
                }
                return out;
            });
    }

    Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string>& requested_object_ids) override {
        return with_tx<std::vector<ObjectPathEntry>>(
            [&](pqxx::work& tx) -> Result<std::vector<ObjectPathEntry>> {
                if (requested_object_ids.empty()) {
                    return std::vector<ObjectPathEntry>{};
                }
                const pqxx::result rows = tx.exec_params(
                    "WITH RECURSIVE walk AS ("
                    "SELECT objects.id AS requested_id, objects.id, objects.object_type, "
                    "objects.external_id, objects.parent_id, objects.inherit, "
                    "objects.created_at, "
                    "objects.updated_at, 0 AS depth FROM objects "
                    "WHERE objects.id = ANY($1::uuid[]) "
                    "UNION ALL "
                    "SELECT walk.requested_id, parent.id, parent.object_type, "
                    "parent.external_id, "
                    "parent.parent_id, parent.inherit, parent.created_at, "
                    "parent.updated_at, "
                    "walk.depth + 1 FROM walk JOIN objects AS parent ON parent.id = "
                    "walk.parent_id "
                    "WHERE walk.inherit) "
                    "SELECT requested_id::text, id::text, object_type, external_id, "
                    "coalesce(parent_id::text,''), inherit, created_at::text, "
                    "updated_at::text, depth "
                    "FROM walk ORDER BY requested_id, depth",
                    uuid_array(requested_object_ids));
                std::vector<ObjectPathEntry> out;
                for (const auto& row : rows) {
                    PolicyObject object{row[1].as<std::string>(),
                                        row[2].as<std::string>(),
                                        row[3].as<std::string>(),
                                        row[4].as<std::string>(),
                                        row[6].as<std::string>(),
                                        row[7].as<std::string>(),
                                        row[5].as<bool>()};
                    out.push_back(ObjectPathEntry{
                        row[0].as<std::string>(), std::move(object), row[8].as<int>()});
                }
                return out;
            });
    }

    Result<Grant> create_grant(const NewGrant& in) override {
        return with_tx<Grant>([&](pqxx::work& tx) -> Result<Grant> {
            const pqxx::result objects =
                tx.exec_params("SELECT object_type FROM objects WHERE id = $1::uuid", in.object_id);
            if (objects.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "object_id does not reference a policy object"});
            }
            if (!role_valid_for_object_type(objects[0][0].as<std::string>(), in.role)) {
                return std::unexpected(
                    StoreError{"invalid_input", "role is not valid for this object type"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO grants (principal_taap_id, object_id, role, "
                            "attestation_digest, granted_by_taap_id) "
                            "VALUES ($1, $2::uuid, $3, $4, $5) RETURNING ") +
                    grant_columns,
                in.principal_taap_id,
                in.object_id,
                in.role,
                in.attestation_digest,
                in.granted_by_taap_id);
            Grant grant = grant_of(rows[0]);
            return grant;
        });
    }

    Result<std::vector<Grant>> list_grants(const std::vector<std::string>& object_ids,
                                           const std::optional<std::string>& principal_taap_id,
                                           int limit) override {
        return with_tx<std::vector<Grant>>([&](pqxx::work& tx) -> Result<std::vector<Grant>> {
            std::string where = " WHERE true";
            pqxx::params params;
            if (!object_ids.empty()) {
                params.append(uuid_array(object_ids));
                where += " AND object_id = ANY($" + std::to_string(params.size()) + "::uuid[])";
            }
            if (principal_taap_id) {
                params.append(*principal_taap_id);
                where += " AND principal_taap_id = $" + std::to_string(params.size());
            }
            params.append(limit);
            const std::string query = std::string("SELECT ") + grant_columns + " FROM grants" +
                                      where + " ORDER BY created_at DESC LIMIT $" +
                                      std::to_string(params.size());
            const pqxx::result rows = tx.exec(query, params);
            std::vector<Grant> out;
            for (const auto& row : rows) {
                out.push_back(grant_of(row));
            }
            return out;
        });
    }

    audit::Result<audit::Identity> current_lake_identity() override {
        return with_audit_tx<audit::Identity>([](pqxx::work& tx) -> audit::Result<audit::Identity> {
            const pqxx::result rows =
                tx.exec(std::string("SELECT ") + identity_columns +
                        " FROM lake_identity WHERE current ORDER BY created_at DESC LIMIT 1");
            if (rows.empty()) {
                return std::unexpected(audit::Error{"not_found", "no lake identity exists"});
            }
            return identity_of(rows[0]);
        });
    }

    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override {
        return with_audit_tx<audit::Identity>(
            [&](pqxx::work& tx) -> audit::Result<audit::Identity> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + identity_columns +
                                       " FROM lake_identity WHERE taap_id = $1",
                                   taap_id);
                if (rows.empty()) {
                    return std::unexpected(
                        audit::Error{"not_found", "no lake identity for " + taap_id});
                }
                return identity_of(rows[0]);
            });
    }

    audit::Result<void> create_lake_identity(const audit::Identity& identity) override {
        return with_audit_tx<void>([&](pqxx::work& tx) -> audit::Result<void> {
            tx.exec_params("INSERT INTO lake_identity "
                           "(id, taap_id, public_key, wrapped_seed, created_at, "
                           "rotation_document, current) "
                           "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, "
                           "NULLIF($6, '')::jsonb, true)",
                           identity.id,
                           identity.taap_id,
                           identity.public_key,
                           identity.wrapped_seed,
                           identity.created_at,
                           identity.rotation_document);
            return {};
        });
    }

    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override {
        return with_audit_tx<void>([&](pqxx::work& tx) -> audit::Result<void> {
            const pqxx::result updated = tx.exec_params("UPDATE lake_identity SET current = false "
                                                        "WHERE id = $1::uuid AND current",
                                                        previous.id);
            if (updated.affected_rows() != 1) {
                return std::unexpected(
                    audit::Error{"conflict", "the lake identity changed before rotation"});
            }
            tx.exec_params("INSERT INTO lake_identity "
                           "(id, taap_id, public_key, wrapped_seed, created_at, "
                           "rotation_document, current) "
                           "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, $6::jsonb, true)",
                           replacement.id,
                           replacement.taap_id,
                           replacement.public_key,
                           replacement.wrapped_seed,
                           replacement.created_at,
                           replacement.rotation_document);
            return {};
        });
    }

    Result<void> append_audit_event(const audit::Event& event) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec("SELECT pg_advisory_xact_lock(hashtext('lake_audit_events'))");
            const pqxx::result previous =
                tx.exec("SELECT digest FROM audit_events ORDER BY sequence DESC LIMIT 1");
            const std::string digest = previous.empty() ? "" : previous[0][0].as<std::string>();
            if (event.previous_digest != digest) {
                return std::unexpected(
                    StoreError{"conflict", "audit event predecessor changed before append"});
            }
            tx.exec_params("INSERT INTO audit_events "
                           "(id, occurred_at, customer_id, kind, principal, object, action, "
                           "outcome, "
                           "request_id, method, device, details, previous_digest, signer, "
                           "signature, digest) "
                           "VALUES ($1::uuid, $2::timestamptz, NULLIF($3, '')::uuid, $4, $5, "
                           "$6, $7, $8, "
                           "$9, NULLIF($10, ''), NULLIF($11, ''), $12::jsonb, NULLIF($13, ''), "
                           "$14, "
                           "$15, $16)",
                           event.id,
                           event.occurred_at,
                           event.draft.customer_id,
                           event.draft.kind,
                           event.draft.principal,
                           event.draft.object,
                           event.draft.action,
                           event.draft.outcome,
                           event.draft.request_id,
                           event.draft.method,
                           event.draft.device,
                           event.draft.details.dump(),
                           event.previous_digest,
                           event.signer,
                           event.signature,
                           event.digest);
            return {};
        });
    }

    Result<audit::Event> latest_audit_event() override {
        return with_tx<audit::Event>([](pqxx::work& tx) -> Result<audit::Event> {
            const pqxx::result rows = tx.exec(std::string("SELECT ") + audit_event_columns +
                                              " FROM audit_events ORDER BY sequence DESC LIMIT 1");
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no audit events exist"});
            }
            return audit_event_of(rows[0]);
        });
    }

    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override {
        return with_tx<std::vector<audit::Event>>(
            [&](pqxx::work& tx) -> Result<std::vector<audit::Event>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + audit_event_columns +
                        " FROM audit_events "
                        "WHERE ($1 = '' OR principal = $1) AND ($2 = '' OR object = $2) "
                        "AND ($3 = '' OR outcome = $3) "
                        "AND ($4 = '' OR occurred_at >= $4::timestamptz) "
                        "AND ($5 = '' OR occurred_at <= $5::timestamptz) "
                        "ORDER BY sequence ASC LIMIT $6",
                    filter.principal,
                    filter.object,
                    filter.outcome,
                    filter.from,
                    filter.to,
                    filter.limit);
                std::vector<audit::Event> events;
                for (const pqxx::row& row : rows) {
                    events.push_back(audit_event_of(row));
                }
                return events;
            });
    }

    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result updated =
                tx.exec_params("INSERT INTO audit_retention (customer_id, retention_days) VALUES "
                               "($1::uuid, $2) "
                               "ON CONFLICT (customer_id) DO UPDATE SET retention_days = "
                               "EXCLUDED.retention_days "
                               "WHERE audit_retention.retention_days <= EXCLUDED.retention_days",
                               customer_id,
                               days);
            if (updated.affected_rows() != 1) {
                return std::unexpected(StoreError{"conflict", "audit retention cannot be reduced"});
            }
            return {};
        });
    }

    Result<IssuedSession> sign_up_password(const NewPasswordUser& in) override {
        if (in.pool_id.empty() || in.email.empty() || in.password.size() < 12) {
            return std::unexpected(StoreError{
                "invalid_input", "pool, email, and a password of 12 characters are required"});
        }
        const Result<VerifiedIdentityDocument> identity =
            verify_identity_document(in.identity_document);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        const std::string password_hash = identity_crypto::hash_secret(in.password);
        if (password_hash.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not derive password credential"});
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result user_rows =
                tx.exec_params("INSERT INTO users (taap_id, email, display_name) "
                               "VALUES ($1, $2, $3) "
                               "RETURNING id::text",
                               identity->identifier,
                               in.email,
                               in.display_name);
            const std::string user_id = user_rows[0][0].as<std::string>();
            tx.exec_params("INSERT INTO identity_documents (user_id, document, "
                           "document_hash) VALUES ($1::uuid, to_jsonb($2::text), $3)",
                           user_id,
                           identity->document,
                           identity->hash);
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, "
                           "subject, secret_hash) "
                           "VALUES ($1::uuid, $2, 'password', lower($3), $4)",
                           user_id,
                           in.pool_id,
                           in.email,
                           password_hash);
            return issue_session(tx,
                                 user_id,
                                 in.pool_id,
                                 in.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }
    Result<IssuedSession> sign_in_password(const PasswordSignIn& in) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result rows =
                tx.exec_params("SELECT user_id::text, secret_hash FROM credentials "
                               "WHERE pool_id = $1 AND kind = 'password' AND subject = lower($2) "
                               "AND revoked_at IS NULL",
                               in.pool_id,
                               in.email);
            if (rows.empty() ||
                !identity_crypto::verify_secret(rows[0][1].as<std::string>(), in.password)) {
                return std::unexpected(StoreError{"not_found", "invalid email or password"});
            }
            const std::string user_id = rows[0][0].as<std::string>();
            const pqxx::result totp_rows = tx.exec_params(
                "SELECT 1 FROM credentials WHERE user_id = $1::uuid AND pool_id = $2 "
                "AND kind = 'totp' AND revoked_at IS NULL",
                user_id,
                in.pool_id);
            if (!totp_rows.empty() && in.totp_code.empty()) {
                return std::unexpected(
                    StoreError{"mfa_required", "a TOTP verification is required"});
            }
            if (!totp_rows.empty()) {
                return sign_in_totp_tx(tx, in, user_id);
            }
            return issue_session(tx,
                                 user_id,
                                 in.pool_id,
                                 in.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }
    Result<void> enroll_totp(const std::string& user_id,
                             const std::string& pool_id,
                             const std::string& secret) override {
        if (secret.empty()) {
            return std::unexpected(StoreError{"invalid_input", "a TOTP seed is required"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const Result<std::string> encrypted = encrypt_secret(tx, "identity.totp", secret);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, subject, "
                           "public_data) "
                           "VALUES ($1::uuid, $2, 'totp', $1, "
                           "jsonb_build_object('encrypted_seed', $3::text)) "
                           "ON CONFLICT (pool_id, kind, subject) DO UPDATE SET "
                           "public_data = EXCLUDED.public_data, revoked_at = NULL",
                           user_id,
                           pool_id,
                           *encrypted);
            return {};
        });
    }
    Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                     const std::string& pool_id) override {
        MfaEnrollment enrollment;
        enrollment.secret = identity_crypto::new_totp_secret();
        for (int index = 0; index < 8; ++index) {
            enrollment.recovery_codes.push_back(identity_crypto::random_secret().substr(0, 12));
        }
        if (enrollment.secret.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create TOTP secret"});
        }
        return with_tx<MfaEnrollment>([&](pqxx::work& tx) -> Result<MfaEnrollment> {
            const Result<std::string> encrypted =
                encrypt_secret(tx, "identity.totp", enrollment.secret);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            tx.exec_params(
                "INSERT INTO credentials (user_id, pool_id, kind, subject, public_data) "
                "VALUES ($1::uuid, $2, 'totp', $1, jsonb_build_object('encrypted_seed', $3::text)) "
                "ON CONFLICT (pool_id, kind, subject) DO UPDATE SET public_data = "
                "EXCLUDED.public_data, revoked_at = NULL",
                user_id,
                pool_id,
                *encrypted);
            tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE user_id = $1::uuid "
                           "AND pool_id = $2 AND kind = 'recovery' AND revoked_at IS NULL",
                           user_id,
                           pool_id);
            for (const std::string& code : enrollment.recovery_codes) {
                tx.exec_params(
                    "INSERT INTO credentials (user_id, pool_id, kind, subject, secret_hash) "
                    "VALUES ($1::uuid, $2, 'recovery', $3, $4)",
                    user_id,
                    pool_id,
                    identity_crypto::random_secret(),
                    identity_crypto::hash_secret(code));
            }
            return enrollment;
        });
    }
    Result<void> disable_mfa(const std::string& user_id,
                             const std::string& pool_id,
                             const std::string& code) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params("SELECT public_data->>'encrypted_seed' FROM credentials "
                               "WHERE user_id = $1::uuid AND pool_id = $2 AND kind = 'totp' "
                               "AND revoked_at IS NULL",
                               user_id,
                               pool_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "MFA is not enrolled"});
            }
            const Result<std::string> seed =
                decrypt_secret(tx, "identity.totp", rows[0][0].as<std::string>());
            if (!seed || !identity_crypto::verify_totp(*seed, code)) {
                return std::unexpected(
                    StoreError{"unauthorized", "a fresh TOTP verification is required"});
            }
            tx.exec_params(
                "UPDATE credentials SET revoked_at = now() WHERE user_id = $1::uuid "
                "AND pool_id = $2 AND kind IN ('totp', 'recovery') AND revoked_at IS NULL",
                user_id,
                pool_id);
            return {};
        });
    }
    Result<IssuedSession> sign_in_totp(const PasswordSignIn& in) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result password_rows =
                tx.exec_params("SELECT user_id::text, secret_hash FROM credentials "
                               "WHERE pool_id = $1 AND kind = 'password' AND subject = lower($2) "
                               "AND revoked_at IS NULL",
                               in.pool_id,
                               in.email);
            if (password_rows.empty() || !identity_crypto::verify_secret(
                                             password_rows[0][1].as<std::string>(), in.password)) {
                return std::unexpected(StoreError{"not_found", "invalid email or password"});
            }
            return sign_in_totp_tx(tx, in, password_rows[0][0].as<std::string>());
        });
    }
    Result<WebAuthnChallenge> begin_passkey_registration(const std::string& user_id,
                                                         const std::string& pool_id,
                                                         const std::string& rp_id,
                                                         const std::string& origin) override {
        if (user_id.empty() || pool_id.empty() || rp_id.empty() || origin.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "WebAuthn registration scope is incomplete"});
        }
        const std::string challenge = identity_crypto::random_secret();
        if (challenge.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not generate WebAuthn challenge"});
        }
        return with_tx<WebAuthnChallenge>([&](pqxx::work& tx) -> Result<WebAuthnChallenge> {
            tx.exec_params("INSERT INTO identity_webauthn_challenges (user_id, pool_id, "
                           "ceremony, challenge, rp_id, origin, expires_at) VALUES "
                           "($1::uuid, $2, 'registration', $3, $4, $5, "
                           "now() + interval '5 minutes')",
                           user_id,
                           pool_id,
                           challenge,
                           rp_id,
                           origin);
            return WebAuthnChallenge{user_id, pool_id, "registration", challenge, rp_id, origin};
        });
    }
    Result<void> register_passkey(const PasskeyRegistration& in) override {
        const Result<WebAuthnChallenge> challenge =
            consume_webauthn_challenge("registration", in.challenge, in.pool_id, in.user_id);
        if (!challenge) {
            return std::unexpected(challenge.error());
        }
        const auto verified = identity_crypto::verify_passkey_registration(in.client_data_json,
                                                                           in.attestation_object,
                                                                           in.challenge,
                                                                           challenge->rp_id,
                                                                           challenge->origin);
        if (!verified) {
            return std::unexpected(StoreError{"unauthorized", verified.error()});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, "
                           "subject, public_data) "
                           "VALUES ($1::uuid, $2, 'passkey', $3, "
                           "jsonb_build_object('public_key', $4::text, 'name', "
                           "$5::text, "
                           "'sign_count', $6::integer))",
                           in.user_id,
                           in.pool_id,
                           verified->credential_id,
                           verified->public_key_pem,
                           in.name,
                           verified->sign_count);
            return {};
        });
    }
    Result<WebAuthnChallenge> begin_passkey_sign_in(const std::string& pool_id,
                                                    const std::string& rp_id,
                                                    const std::string& origin) override {
        if (pool_id.empty() || rp_id.empty() || origin.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "WebAuthn assertion scope is incomplete"});
        }
        const std::string challenge = identity_crypto::random_secret();
        if (challenge.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not generate WebAuthn challenge"});
        }
        return with_tx<WebAuthnChallenge>([&](pqxx::work& tx) -> Result<WebAuthnChallenge> {
            tx.exec_params("INSERT INTO identity_webauthn_challenges (pool_id, ceremony, "
                           "challenge, rp_id, origin, expires_at) VALUES "
                           "($1, 'assertion', $2, $3, $4, now() + interval '5 minutes')",
                           pool_id,
                           challenge,
                           rp_id,
                           origin);
            return WebAuthnChallenge{"", pool_id, "assertion", challenge, rp_id, origin};
        });
    }
    Result<IssuedSession> sign_in_passkey(const PasskeyAssertion& in) override {
        const Result<WebAuthnChallenge> challenge =
            consume_webauthn_challenge("assertion", in.challenge, in.pool_id, "");
        if (!challenge) {
            return std::unexpected(challenge.error());
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result rows =
                tx.exec_params("SELECT user_id::text, public_data->>'public_key', "
                               "COALESCE((public_data->>'sign_count')::bigint, 0) "
                               "FROM credentials WHERE pool_id = $1 AND kind = 'passkey' "
                               "AND subject = $2 AND revoked_at IS NULL FOR UPDATE",
                               in.pool_id,
                               in.credential_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "passkey is not registered"});
            }
            const std::uint32_t previous_count = rows[0][2].as<std::uint32_t>();
            const auto sign_count =
                identity_crypto::verify_passkey_assertion(in.client_data_json,
                                                          in.authenticator_data,
                                                          in.signature,
                                                          rows[0][1].as<std::string>(),
                                                          in.challenge,
                                                          challenge->rp_id,
                                                          challenge->origin);
            if (!sign_count) {
                return std::unexpected(StoreError{"unauthorized", sign_count.error()});
            }
            if ((*sign_count != 0 || previous_count != 0) && *sign_count <= previous_count) {
                return std::unexpected(
                    StoreError{"unauthorized", "passkey sign count did not advance"});
            }
            tx.exec_params("UPDATE credentials SET public_data = jsonb_set(public_data, "
                           "'{sign_count}', to_jsonb($3::bigint), true) WHERE pool_id = $1 "
                           "AND kind = 'passkey' AND subject = $2",
                           in.pool_id,
                           in.credential_id,
                           *sign_count);
            const std::string user_id = rows[0][0].as<std::string>();
            return issue_session(tx,
                                 user_id,
                                 in.pool_id,
                                 in.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }
    Result<IssuedSession> sign_in_oauth(const OAuthIdentity& in) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const std::string subject = in.provider + ":" + in.subject;
            pqxx::result rows =
                tx.exec_params("SELECT user_id::text FROM credentials WHERE pool_id = $1 "
                               "AND kind = 'oauth' AND subject = $2 AND revoked_at IS NULL",
                               in.pool_id,
                               subject);
            std::string user_id;
            if (rows.empty()) {
                const Result<VerifiedIdentityDocument> identity =
                    verify_identity_document(in.identity_document);
                if (!identity) {
                    return std::unexpected(identity.error());
                }
                rows = tx.exec_params("INSERT INTO users (taap_id, email, display_name) VALUES "
                                      "($1, $2, $3) RETURNING id::text",
                                      identity->identifier,
                                      nullable(in.email),
                                      in.display_name);
                user_id = rows[0][0].as<std::string>();
                tx.exec_params("INSERT INTO identity_documents (user_id, document, "
                               "document_hash) VALUES ($1::uuid, to_jsonb($2::text), $3)",
                               user_id,
                               identity->document,
                               identity->hash);
                tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, subject) "
                               "VALUES ($1::uuid, $2, 'oauth', $3)",
                               user_id,
                               in.pool_id,
                               subject);
            } else {
                user_id = rows[0][0].as<std::string>();
            }
            return issue_session(tx,
                                 user_id,
                                 in.pool_id,
                                 in.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }
    Result<void> register_oauth_provider(const OAuthProvider& in) override {
        if (in.pool_id.empty() || in.provider.empty() || in.issuer.empty() ||
            in.client_id.empty() || in.client_secret.empty()) {
            return std::unexpected(
                StoreError{"invalid_input",
                           "pool, provider, issuer, client id, and client secret are required"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const Result<std::string> encrypted = encrypt_secret(
                tx, "identity.oauth." + in.pool_id + "." + in.provider, in.client_secret);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            tx.exec_params("INSERT INTO oauth_providers (pool_id, provider, issuer, client_id, "
                           "encrypted_client_secret, scopes) VALUES ($1, $2, $3, $4, decode($5, "
                           "'hex'), "
                           "ARRAY(SELECT jsonb_array_elements_text($6::jsonb))) "
                           "ON CONFLICT (pool_id, provider) DO UPDATE SET issuer = "
                           "EXCLUDED.issuer, "
                           "client_id = EXCLUDED.client_id, encrypted_client_secret = "
                           "EXCLUDED.encrypted_client_secret, "
                           "scopes = EXCLUDED.scopes",
                           in.pool_id,
                           in.provider,
                           in.issuer,
                           in.client_id,
                           *encrypted,
                           nlohmann::json(in.scopes).dump());
            return {};
        });
    }
    Result<OAuthProvider> get_oauth_provider(const std::string& pool_id,
                                             const std::string& provider) override {
        return with_tx<OAuthProvider>([&](pqxx::work& tx) -> Result<OAuthProvider> {
            const pqxx::result rows =
                tx.exec_params("SELECT issuer, client_id, encode(encrypted_client_secret, "
                               "'hex'), array_to_json(scopes)::text FROM oauth_providers "
                               "WHERE pool_id = $1 AND provider = $2",
                               pool_id,
                               provider);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "OAuth provider is not registered"});
            }
            const Result<std::string> secret = decrypt_secret(
                tx, "identity.oauth." + pool_id + "." + provider, rows[0][2].as<std::string>());
            if (!secret) {
                return std::unexpected(secret.error());
            }
            return OAuthProvider{pool_id,
                                 provider,
                                 rows[0][0].as<std::string>(),
                                 rows[0][1].as<std::string>(),
                                 *secret,
                                 nlohmann::json::parse(rows[0][3].as<std::string>())
                                     .get<std::vector<std::string>>()};
        });
    }
    Result<void> begin_oauth(const OAuthFlow& in) override {
        if (in.pool_id.empty() || in.provider.empty() || in.state.empty() ||
            in.code_verifier.empty() || in.nonce.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "OAuth flow material is incomplete"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const Result<std::string> encrypted =
                encrypt_secret(tx, "identity.oauth.flow", in.code_verifier);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            const std::string state_hash = identity_crypto::sha256_hex(in.state);
            if (state_hash.empty()) {
                return std::unexpected(StoreError{"store_error", "could not hash OAuth state"});
            }
            tx.exec_params("INSERT INTO identity_oauth_flows (pool_id, provider, state_hash, "
                           "encrypted_code_verifier, nonce, device_name, identity_document, "
                           "expires_at) VALUES ($1, $2, $3, decode($4, 'hex'), $5, $6, $7, "
                           "now() + interval '5 minutes')",
                           in.pool_id,
                           in.provider,
                           state_hash,
                           *encrypted,
                           in.nonce,
                           in.device_name,
                           in.identity_document);
            return {};
        });
    }
    Result<OAuthFlow> consume_oauth(const std::string& provider,
                                    const std::string& state) override {
        return with_tx<OAuthFlow>([&](pqxx::work& tx) -> Result<OAuthFlow> {
            const std::string state_hash = identity_crypto::sha256_hex(state);
            if (state_hash.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "OAuth state is invalid or expired"});
            }
            const pqxx::result rows =
                tx.exec_params("UPDATE identity_oauth_flows SET used_at = now() WHERE provider = "
                               "$1 AND state_hash = $2 AND used_at IS NULL AND expires_at > now() "
                               "RETURNING pool_id, encode(encrypted_code_verifier, 'hex'), nonce, "
                               "device_name, identity_document",
                               provider,
                               state_hash);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "OAuth state is invalid or expired"});
            }
            const Result<std::string> verifier =
                decrypt_secret(tx, "identity.oauth.flow", rows[0][1].as<std::string>());
            if (!verifier) {
                return std::unexpected(verifier.error());
            }
            return OAuthFlow{rows[0][0].as<std::string>(),
                             provider,
                             state,
                             *verifier,
                             rows[0][2].as<std::string>(),
                             rows[0][3].as<std::string>(),
                             rows[0][4].as<std::string>()};
        });
    }
    Result<IssuedSession> create_token(const NewToken& in) override {
        if (in.scopes.empty() || (in.is_static && in.reason.empty())) {
            return std::unexpected(
                StoreError{"invalid_input", "a token scope and static-token reason are required"});
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const int lifetime = in.is_static ? 0
                                              : (in.expires_in_seconds > 0 ? in.expires_in_seconds
                                                                           : 60 * 60 * 24 * 30);
            return issue_session(tx,
                                 in.user_id,
                                 in.pool_id,
                                 "",
                                 in.is_static ? "static" : "api_token",
                                 in.name,
                                 in.scopes,
                                 in.reason,
                                 lifetime);
        });
    }
    Result<std::vector<Session>> list_sessions(const std::string& user_id,
                                               const std::string& pool_id) override {
        return with_tx<std::vector<Session>>([&](pqxx::work& tx) -> Result<std::vector<Session>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + session_columns +
                                   " FROM sessions WHERE user_id = $1::uuid "
                                   "AND pool_id = $2 ORDER BY created_at DESC",
                               user_id,
                               pool_id);
            std::vector<Session> sessions;
            for (const pqxx::row& row : rows) {
                sessions.push_back(session_of(row));
            }
            return sessions;
        });
    }
    Result<void> revoke_session(const std::string& user_id,
                                const std::string& session_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params("UPDATE sessions SET revoked_at = now() WHERE id = $1::uuid "
                               "AND user_id = $2::uuid AND revoked_at IS NULL RETURNING id",
                               session_id,
                               user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            return {};
        });
    }
    Result<std::vector<Device>> list_devices(const std::string& user_id) override {
        return with_tx<std::vector<Device>>([&](pqxx::work& tx) -> Result<std::vector<Device>> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + device_columns +
                                                         " FROM devices WHERE user_id = $1::uuid "
                                                         "ORDER BY created_at DESC",
                                                     user_id);
            std::vector<Device> devices;
            for (const pqxx::row& row : rows) {
                devices.push_back(device_of(row));
            }
            return devices;
        });
    }
    Result<void> revoke_device(const std::string& user_id, const std::string& device_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params("UPDATE devices SET revoked_at = now() WHERE id = $1::uuid "
                               "AND user_id = $2::uuid AND revoked_at IS NULL RETURNING id",
                               device_id,
                               user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "device is not active"});
            }
            return {};
        });
    }
    Result<Principal> context_for(const std::string& secret) override {
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows =
                tx.exec(std::string("SELECT ") + qualified_session_columns +
                        ", COALESCE(users.taap_id, ''), secret_hash FROM sessions "
                        "LEFT JOIN devices ON devices.id = sessions.device_id "
                        "LEFT JOIN users ON users.id = sessions.user_id "
                        "WHERE sessions.revoked_at IS NULL AND (sessions.expires_at IS NULL "
                        "OR sessions.expires_at > now()) AND (devices.id IS NULL OR "
                        "devices.revoked_at IS NULL)");
            for (const pqxx::row& row : rows) {
                const std::string secret_hash = row[13].as<std::string>();
                if (!identity_crypto::verify_secret(secret_hash, secret)) {
                    continue;
                }
                const Session session = session_of(row);
                tx.exec_params("UPDATE sessions SET last_used_at = now() WHERE id = $1::uuid",
                               session.id);
                return Principal{session.user_id,
                                 row[12].as<std::string>(),
                                 session.id,
                                 session.device_id,
                                 session.pool_id,
                                 session.kind,
                                 session.scopes};
            }
            return std::unexpected(StoreError{"not_found", "session is not active"});
        });
    }

private:
    Result<WebAuthnChallenge> consume_webauthn_challenge(const std::string& ceremony,
                                                         const std::string& challenge,
                                                         const std::string& pool_id,
                                                         const std::string& user_id) {
        return with_tx<WebAuthnChallenge>([&](pqxx::work& tx) -> Result<WebAuthnChallenge> {
            const pqxx::result rows =
                tx.exec_params("UPDATE identity_webauthn_challenges SET used_at = now() "
                               "WHERE ceremony = $1 AND challenge = $2 AND pool_id = $3 "
                               "AND (NULLIF($4::text, '') IS NULL OR "
                               "user_id = NULLIF($4::text, '')::uuid) "
                               "AND used_at IS NULL AND expires_at > now() RETURNING "
                               "COALESCE(user_id::text, ''), pool_id, ceremony, challenge, "
                               "rp_id, origin",
                               ceremony,
                               challenge,
                               pool_id,
                               user_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "WebAuthn challenge is invalid or expired"});
            }
            return WebAuthnChallenge{rows[0][0].as<std::string>(),
                                     rows[0][1].as<std::string>(),
                                     rows[0][2].as<std::string>(),
                                     rows[0][3].as<std::string>(),
                                     rows[0][4].as<std::string>(),
                                     rows[0][5].as<std::string>()};
        });
    }

    Result<VerifiedIdentityDocument> verify_identity_document(std::string_view serialized) const {
        const auto encoded = decode_base64url(serialized);
        if (!encoded) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document must be base64url CBOR"});
        }
        const auto document = liquid::decode_identity_document(*encoded);
        if (!document) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document is invalid canonical CBOR"});
        }
        const auto verified = liquid::verify_document(*document);
        if (!verified || !*verified) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document signature is invalid"});
        }
        const auto canonical = liquid::encode_identity_document(*document);
        if (!canonical || *canonical != *encoded) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document is not canonical"});
        }
        {
            EVP_MD* algorithm = EVP_MD_fetch(nullptr, "SHA512-256", nullptr);
            EVP_MD_CTX* digest_context = EVP_MD_CTX_new();
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int digest_size = 0;
            const int hashed =
                algorithm != nullptr && digest_context != nullptr &&
                EVP_DigestInit_ex(digest_context, algorithm, nullptr) &&
                EVP_DigestUpdate(digest_context, canonical->data(), canonical->size()) &&
                EVP_DigestFinal_ex(digest_context, digest.data(), &digest_size);
            EVP_MD_CTX_free(digest_context);
            EVP_MD_free(algorithm);
            if (hashed != 1) {
                return std::unexpected(
                    StoreError{"store_error", "could not hash TAAP identity document"});
            }
            return VerifiedIdentityDocument{document->subject.taap_id,
                                            std::string(serialized),
                                            hex_bytes(digest.data(), digest_size)};
        }
    }

    Result<IssuedSession> issue_session(pqxx::work& tx,
                                        const std::string& user_id,
                                        const std::string& pool_id,
                                        const std::string& device_name,
                                        const std::string& kind,
                                        const std::string& name,
                                        const std::vector<std::string>& scopes,
                                        const std::string& reason,
                                        int lifetime_seconds) {
        const std::string secret = identity_crypto::random_secret();
        const std::string secret_hash = identity_crypto::hash_secret(secret);
        if (secret.empty() || secret_hash.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not derive session credential"});
        }
        std::string device_id;
        if (!device_name.empty()) {
            const pqxx::result devices =
                tx.exec_params("INSERT INTO devices (user_id, name) VALUES "
                               "($1::uuid, $2) RETURNING id::text",
                               user_id,
                               device_name);
            device_id = devices[0][0].as<std::string>();
        }
        const pqxx::result sessions =
            tx.exec_params(std::string("INSERT INTO sessions (user_id, device_id, pool_id, kind, "
                                       "name, secret_hash, ") +
                               "scopes, reason, expires_at) VALUES ($1::uuid, $2::uuid, $3, $4, "
                               "$5, $6, "
                               "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, "
                               "CASE WHEN $9 > 0 THEN now() + make_interval(secs => $9) ELSE NULL "
                               "END) RETURNING " +
                               session_columns,
                           user_id,
                           nullable(device_id),
                           pool_id,
                           kind,
                           name,
                           secret_hash,
                           nlohmann::json(scopes).dump(),
                           reason,
                           lifetime_seconds);
        const Session session = session_of(sessions[0]);
        return IssuedSession{session, secret};
    }

    Result<IssuedSession>
    sign_in_totp_tx(pqxx::work& tx, const PasswordSignIn& in, const std::string& user_id) {
        const pqxx::result rows =
            tx.exec_params("SELECT public_data->>'encrypted_seed' FROM credentials WHERE user_id "
                           "= $1::uuid "
                           "AND pool_id = $2 AND kind = 'totp' AND revoked_at IS NULL",
                           user_id,
                           in.pool_id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "invalid TOTP code"});
        }
        const Result<std::string> seed =
            decrypt_secret(tx, "identity.totp", rows[0][0].as<std::string>());
        if (!seed) {
            return std::unexpected(seed.error());
        }
        if (!identity_crypto::verify_totp(*seed, in.totp_code)) {
            const pqxx::result recovery_rows = tx.exec_params(
                "SELECT id::text, secret_hash FROM credentials WHERE user_id = $1::uuid "
                "AND pool_id = $2 AND kind = 'recovery' AND revoked_at IS NULL",
                user_id,
                in.pool_id);
            auto recovery =
                std::find_if(recovery_rows.begin(), recovery_rows.end(), [&](const pqxx::row& row) {
                    return identity_crypto::verify_secret(row[1].as<std::string>(), in.totp_code);
                });
            if (recovery == recovery_rows.end()) {
                return std::unexpected(StoreError{"not_found", "invalid TOTP code"});
            }
            tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE id = $1::uuid",
                           (*recovery)[0].as<std::string>());
        }
        return issue_session(
            tx, user_id, in.pool_id, in.device_name, "browser", "", {"*"}, "", 60 * 60 * 24 * 30);
    }

    Result<std::array<unsigned char, 32>> instance_kek() const {
        const char* configured = std::getenv("LAKE_INSTANCE_KEK");
        if (configured == nullptr) {
            return std::unexpected(StoreError{
                "store_error", "LAKE_INSTANCE_KEK is required for encrypted credentials"});
        }
        const auto decoded = decode_hex(configured);
        if (!decoded || decoded->size() != 32) {
            return std::unexpected(
                StoreError{"store_error", "LAKE_INSTANCE_KEK must be 32 bytes of hexadecimal"});
        }
        std::array<unsigned char, 32> key{};
        std::copy(decoded->begin(), decoded->end(), key.begin());
        return key;
    }

    Result<std::string>
    encrypt_secret(pqxx::work& tx, const std::string& purpose, std::string_view plaintext) {
        const Result<std::array<unsigned char, 32>> key = data_key_for(tx, purpose);
        if (!key) {
            return std::unexpected(key.error());
        }
        std::array<unsigned char, 12> nonce{};
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
            return std::unexpected(StoreError{"store_error", "could not generate an AEAD nonce"});
        }
        const Result<std::vector<unsigned char>> encrypted =
            aes_gcm_encrypt(*key, nonce, plaintext);
        if (!encrypted) {
            return std::unexpected(encrypted.error());
        }
        return hex_bytes(nonce.data(), nonce.size()) +
               hex_bytes(encrypted->data(), encrypted->size());
    }

    Result<std::string>
    decrypt_secret(pqxx::work& tx, const std::string& purpose, std::string_view encoded) {
        const auto bytes = decode_hex(encoded);
        if (!bytes || bytes->size() < 28) {
            return std::unexpected(StoreError{"store_error", "encrypted credential is malformed"});
        }
        const Result<std::array<unsigned char, 32>> key = data_key_for(tx, purpose);
        if (!key) {
            return std::unexpected(key.error());
        }
        std::array<unsigned char, 12> nonce{};
        std::copy_n(bytes->begin(), nonce.size(), nonce.begin());
        return aes_gcm_decrypt(*key,
                               nonce,
                               std::span<const unsigned char>(bytes->data() + nonce.size(),
                                                              bytes->size() - nonce.size()));
    }

    Result<std::array<unsigned char, 32>> data_key_for(pqxx::work& tx, const std::string& purpose) {
        const Result<std::array<unsigned char, 32>> kek = instance_kek();
        if (!kek) {
            return std::unexpected(kek.error());
        }
        const pqxx::result rows = tx.exec_params(
            "SELECT encode(wrapped_key, 'hex') FROM keystore WHERE purpose = $1", purpose);
        if (!rows.empty()) {
            const auto wrapped = decode_hex(rows[0][0].as<std::string>());
            if (!wrapped || wrapped->size() < 28) {
                return std::unexpected(StoreError{"store_error", "wrapped data key is malformed"});
            }
            std::array<unsigned char, 12> nonce{};
            std::copy_n(wrapped->begin(), nonce.size(), nonce.begin());
            const Result<std::string> data_key =
                aes_gcm_decrypt(*kek,
                                nonce,
                                std::span<const unsigned char>(wrapped->data() + nonce.size(),
                                                               wrapped->size() - nonce.size()));
            if (!data_key || data_key->size() != 32) {
                return std::unexpected(
                    StoreError{"store_error", "wrapped data key cannot be opened"});
            }
            std::array<unsigned char, 32> result{};
            std::copy_n(data_key->begin(), result.size(), result.begin());
            return result;
        }
        std::array<unsigned char, 32> data_key{};
        std::array<unsigned char, 12> nonce{};
        if (RAND_bytes(data_key.data(), static_cast<int>(data_key.size())) != 1 ||
            RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
            return std::unexpected(
                StoreError{"store_error", "could not generate an encrypted data key"});
        }
        const Result<std::vector<unsigned char>> wrapped = aes_gcm_encrypt(
            *kek,
            nonce,
            std::string_view(reinterpret_cast<const char*>(data_key.data()), data_key.size()));
        if (!wrapped) {
            return std::unexpected(wrapped.error());
        }
        const std::string encoded =
            hex_bytes(nonce.data(), nonce.size()) + hex_bytes(wrapped->data(), wrapped->size());
        tx.exec_params("INSERT INTO keystore (purpose, wrapped_key, version) "
                       "VALUES ($1, decode($2, 'hex'), 1)",
                       purpose,
                       encoded);
        return data_key;
    }

    Result<std::vector<unsigned char>> aes_gcm_encrypt(const std::array<unsigned char, 32>& key,
                                                       const std::array<unsigned char, 12>& nonce,
                                                       std::string_view plaintext) const {
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        if (context == nullptr) {
            return std::unexpected(StoreError{"store_error", "could not allocate an AEAD context"});
        }
        std::vector<unsigned char> result(plaintext.size() + 16);
        std::array<unsigned char, 16> tag{};
        int written = 0;
        int finalized = 0;
        const int ok =
            EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) &&
            EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) &&
            EVP_EncryptUpdate(context,
                              result.data(),
                              &written,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) &&
            EVP_EncryptFinal_ex(context, result.data() + written, &finalized) &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data());
        EVP_CIPHER_CTX_free(context);
        if (ok != 1) {
            return std::unexpected(StoreError{"store_error", "could not encrypt credential data"});
        }
        result.resize(static_cast<std::size_t>(written + finalized));
        result.insert(result.end(), tag.begin(), tag.end());
        return result;
    }

    Result<std::string> aes_gcm_decrypt(const std::array<unsigned char, 32>& key,
                                        const std::array<unsigned char, 12>& nonce,
                                        std::span<const unsigned char> ciphertext) const {
        if (ciphertext.size() < 16) {
            return std::unexpected(StoreError{"store_error", "encrypted credential is malformed"});
        }
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        if (context == nullptr) {
            return std::unexpected(StoreError{"store_error", "could not allocate an AEAD context"});
        }
        const std::size_t encrypted_size = ciphertext.size() - 16;
        std::string result(encrypted_size, '\0');
        int written = 0;
        int finalized = 0;
        const int ok =
            EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) &&
            EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) &&
            EVP_DecryptUpdate(context,
                              reinterpret_cast<unsigned char*>(result.data()),
                              &written,
                              ciphertext.data(),
                              static_cast<int>(encrypted_size)) &&
            EVP_CIPHER_CTX_ctrl(context,
                                EVP_CTRL_GCM_SET_TAG,
                                16,
                                const_cast<unsigned char*>(ciphertext.data() + encrypted_size)) &&
            EVP_DecryptFinal_ex(
                context, reinterpret_cast<unsigned char*>(result.data()) + written, &finalized);
        EVP_CIPHER_CTX_free(context);
        if (ok != 1) {
            return std::unexpected(
                StoreError{"store_error", "encrypted credential authentication failed"});
        }
        result.resize(static_cast<std::size_t>(written + finalized));
        return result;
    }

    template <class T, class F> audit::Result<T> with_audit_tx(F&& body) {
        std::lock_guard lock(mutex_);
        try {
            if (!connection_ || !connection_->is_open())
                connection_ = std::make_unique<pqxx::connection>(uri_);
            pqxx::work tx(*connection_);
            audit::Result<T> result = body(tx);
            if (result)
                tx.commit();
            return result;
        } catch (const pqxx::unique_violation& error) {
            return std::unexpected(audit::Error{"conflict", error.what()});
        } catch (const pqxx::broken_connection& error) {
            connection_.reset();
            return std::unexpected(audit::Error{"store_unavailable", error.what()});
        } catch (const std::exception& error) {
            return std::unexpected(audit::Error{"store_error", error.what()});
        }
    }
    template <class T, class F> Result<T> with_tx(F&& body) {
        std::lock_guard lock(mutex_);
        try {
            if (!connection_ || !connection_->is_open())
                connection_ = std::make_unique<pqxx::connection>(uri_);
            pqxx::work tx(*connection_);
            Result<T> result = body(tx);
            if (result)
                tx.commit();
            return result;
        } catch (const pqxx::broken_connection& error) {
            connection_.reset();
            return std::unexpected(StoreError{"store_unavailable", error.what()});
        } catch (const pqxx::unique_violation& error) {
            return std::unexpected(StoreError{"conflict", error.what()});
        } catch (const pqxx::check_violation& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const pqxx::foreign_key_violation& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const pqxx::data_exception& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const std::exception& error) {
            return std::unexpected(StoreError{"store_error", error.what()});
        }
    }

    std::string uri_;
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> connection_;
};

} // namespace

std::unique_ptr<Store> make_postgres_store(std::string connection_uri) {
    return std::make_unique<PostgresStore>(std::move(connection_uri));
}

} // namespace lake::control

// Audit cryptography is intentionally compiled through this provider-bound
// translation unit. The audit API remains provider-neutral while
// external-library names stay confined to the designated server and
// control-store translation units.
#include "../audit/audit.cpp"
