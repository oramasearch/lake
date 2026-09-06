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
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/platform_store.hpp"
#include "secrets_crypto.hpp"

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

std::expected<VerifiedTaapSignIn, std::string>
verify_taap_sign_in(std::string_view identity_document,
                    std::string_view pool_id,
                    std::string_view nonce,
                    std::int64_t issued_at,
                    const nlohmann::json& signature,
                    std::int64_t now_unix) {
    if (pool_id.empty() || nonce.size() < 16 || !signature.is_object()) {
        return std::unexpected("TAAP sign-in fields are incomplete");
    }
    if (now_unix == 0) {
        now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    }
    if (issued_at < now_unix - 300 || issued_at > now_unix + 60) {
        return std::unexpected("TAAP sign-in assertion is outside its validity window");
    }
    const std::vector<unsigned char> document_bytes = from_base64url(identity_document);
    const std::vector<unsigned char> signature_bytes = from_base64url(signature.value("bytes", ""));
    if (document_bytes.empty() || signature_bytes.empty()) {
        return std::unexpected("TAAP sign-in document or signature is not base64url");
    }
    std::vector<std::byte> encoded_document(document_bytes.size());
    std::vector<std::byte> encoded_signature(signature_bytes.size());
    std::transform(document_bytes.begin(),
                   document_bytes.end(),
                   encoded_document.begin(),
                   [](auto value) { return std::byte{value}; });
    std::transform(signature_bytes.begin(),
                   signature_bytes.end(),
                   encoded_signature.begin(),
                   [](auto value) { return std::byte{value}; });
    const auto document = liquid::decode_identity_document(encoded_document);
    if (!document) {
        return std::unexpected("TAAP identity document is invalid canonical CBOR");
    }
    const auto verified_document = liquid::verify_document(*document);
    const auto canonical = liquid::encode_identity_document(*document);
    if (!verified_document || !*verified_document || !canonical || *canonical != encoded_document) {
        return std::unexpected("TAAP identity document does not verify canonically");
    }
    liquid::signature signed_value{signature.value("algorithm", ""),
                                   signature.value("signing_key_identifier", ""),
                                   signature.value("signing_key_label", ""),
                                   signature.value("signature_purpose", ""),
                                   signature.value("signed_at", ""),
                                   std::move(encoded_signature)};
    const std::string body =
        std::string(pool_id) + "\n" + std::string(nonce) + "\n" + std::to_string(issued_at);
    const std::span<const std::byte> body_bytes(reinterpret_cast<const std::byte*>(body.data()),
                                                body.size());
    const auto verified =
        liquid::verify(document->subject, "lake_sign_in", body_bytes, signed_value);
    if (!verified || !*verified) {
        return std::unexpected("TAAP sign-in signature does not verify");
    }
    return VerifiedTaapSignIn{
        document->subject.taap_id,
        sha256_hex(std::string(identity_document) + "\n" + body + "\n" + signature.dump())};
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

namespace secrets_crypto {
namespace {

std::string encoded_bytes(std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
        return {};
    }
    std::string output(((bytes.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     bytes.data(),
                                     static_cast<int>(bytes.size()));
    if (size < 0) {
        return {};
    }
    output.resize(static_cast<std::size_t>(size));
    while (output.ends_with('=')) {
        output.pop_back();
    }
    std::replace(output.begin(), output.end(), '+', '-');
    std::replace(output.begin(), output.end(), '/', '_');
    return output;
}

Result<std::vector<unsigned char>> decoded_bytes(std::string_view value) {
    std::string padded(value);
    if (padded.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
        std::string::npos) {
        return std::unexpected(StoreError{"invalid_input", "value is not base64url"});
    }
    std::replace(padded.begin(), padded.end(), '-', '+');
    std::replace(padded.begin(), padded.end(), '_', '/');
    while (padded.size() % 4 != 0) {
        padded.push_back('=');
    }
    if (padded.empty()) {
        return std::vector<unsigned char>{};
    }
    std::vector<unsigned char> output((padded.size() / 4) * 3);
    const int size = EVP_DecodeBlock(output.data(),
                                     reinterpret_cast<const unsigned char*>(padded.data()),
                                     static_cast<int>(padded.size()));
    if (size < 0) {
        return std::unexpected(StoreError{"invalid_input", "value is not base64url"});
    }
    std::size_t padding = 0;
    if (padded.ends_with('=')) {
        ++padding;
    }
    if (padded.size() > 1 && padded[padded.size() - 2] == '=') {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

Result<std::vector<unsigned char>> encrypt_bytes(const Key& key,
                                                 std::span<const unsigned char> nonce,
                                                 std::string_view plaintext,
                                                 std::string_view associated) {
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (raw == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not allocate an AEAD context"});
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw,
                                                                            EVP_CIPHER_CTX_free);
    std::vector<unsigned char> output(plaintext.size() + 16);
    std::array<unsigned char, 16> tag{};
    int ignored = 0;
    int written = 0;
    int finalized = 0;
    const int initialized =
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
        EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) &&
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data());
    const int associated_ok =
        associated.empty() ||
        EVP_EncryptUpdate(context.get(),
                          nullptr,
                          &ignored,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size()));
    const int encrypted =
        EVP_EncryptUpdate(context.get(),
                          output.data(),
                          &written,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) &&
        EVP_EncryptFinal_ex(context.get(), output.data() + written, &finalized) &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data());
    if (initialized != 1 || associated_ok != 1 || encrypted != 1) {
        return std::unexpected(StoreError{"store_error", "could not encrypt protected data"});
    }
    output.resize(static_cast<std::size_t>(written + finalized));
    output.insert(output.end(), tag.begin(), tag.end());
    return output;
}

Result<std::string> decrypt_bytes(const Key& key,
                                  std::span<const unsigned char> nonce,
                                  std::span<const unsigned char> ciphertext,
                                  std::string_view associated) {
    if (ciphertext.size() < 16) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is malformed"});
    }
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (raw == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not allocate an AEAD context"});
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw,
                                                                            EVP_CIPHER_CTX_free);
    const std::size_t encrypted_size = ciphertext.size() - 16;
    std::string output(encrypted_size, '\0');
    int ignored = 0;
    int written = 0;
    int finalized = 0;
    const int initialized =
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
        EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) &&
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data());
    const int associated_ok =
        associated.empty() ||
        EVP_DecryptUpdate(context.get(),
                          nullptr,
                          &ignored,
                          reinterpret_cast<const unsigned char*>(associated.data()),
                          static_cast<int>(associated.size()));
    const int decrypted =
        EVP_DecryptUpdate(context.get(),
                          reinterpret_cast<unsigned char*>(output.data()),
                          &written,
                          ciphertext.data(),
                          static_cast<int>(encrypted_size)) &&
        EVP_CIPHER_CTX_ctrl(context.get(),
                            EVP_CTRL_GCM_SET_TAG,
                            16,
                            const_cast<unsigned char*>(ciphertext.data() + encrypted_size)) &&
        EVP_DecryptFinal_ex(
            context.get(), reinterpret_cast<unsigned char*>(output.data()) + written, &finalized);
    if (initialized != 1 || associated_ok != 1 || decrypted != 1) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext authentication failed"});
    }
    output.resize(static_cast<std::size_t>(written + finalized));
    return output;
}

} // namespace

Result<Key> instance_key(std::string_view encoded) {
    if (encoded.size() != 64) {
        return std::unexpected(
            StoreError{"store_error", "LAKE_INSTANCE_KEK must be 32 bytes of hexadecimal"});
    }
    Key key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        unsigned parsed = 0;
        const auto [end, error] =
            std::from_chars(encoded.data() + index * 2, encoded.data() + index * 2 + 2, parsed, 16);
        if (error != std::errc{} || end != encoded.data() + index * 2 + 2) {
            cleanse(key);
            return std::unexpected(
                StoreError{"store_error", "LAKE_INSTANCE_KEK must be 32 bytes of hexadecimal"});
        }
        key[index] = static_cast<unsigned char>(parsed);
    }
    return key;
}

Result<Key> random_key() {
    Key key{};
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
        return std::unexpected(StoreError{"store_error", "could not generate key material"});
    }
    return key;
}

Result<Key> derive(const Key& key, std::string_view context) {
    Key output{};
    unsigned int size = 0;
    const unsigned char* result = HMAC(EVP_sha256(),
                                       key.data(),
                                       static_cast<int>(key.size()),
                                       reinterpret_cast<const unsigned char*>(context.data()),
                                       context.size(),
                                       output.data(),
                                       &size);
    if (result == nullptr || size != output.size()) {
        cleanse(output);
        return std::unexpected(StoreError{"store_error", "could not derive key material"});
    }
    return output;
}

Result<std::string> seal(const Key& key, std::string_view plaintext, std::string_view associated) {
    std::array<unsigned char, 12> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return std::unexpected(StoreError{"store_error", "could not generate an AEAD nonce"});
    }
    auto encrypted = encrypt_bytes(key, nonce, plaintext, associated);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    std::vector<unsigned char> value(nonce.begin(), nonce.end());
    value.insert(value.end(), encrypted->begin(), encrypted->end());
    return encoded_bytes(value);
}

Result<std::string> open(const Key& key, std::string_view sealed, std::string_view associated) {
    auto value = decoded_bytes(sealed);
    if (!value || value->size() < 28) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is malformed"});
    }
    return decrypt_bytes(key,
                         std::span<const unsigned char>(value->data(), 12),
                         std::span<const unsigned char>(value->data() + 12, value->size() - 12),
                         associated);
}

Result<std::string> base64url_encode(std::string_view value) {
    return encoded_bytes(
        std::span(reinterpret_cast<const unsigned char*>(value.data()), value.size()));
}

Result<std::string> base64url_decode(std::string_view value) {
    auto decoded = decoded_bytes(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return std::string(reinterpret_cast<const char*>(decoded->data()), decoded->size());
}

Result<std::string> sign(const Key& key, std::string_view message) {
    EVP_PKEY* raw = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
    if (raw == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not create a signing key"});
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> signing_key(raw, EVP_PKEY_free);
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not allocate a signing context"});
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
    std::size_t size = 0;
    if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, signing_key.get()) != 1 ||
        EVP_DigestSign(context.get(),
                       nullptr,
                       &size,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return std::unexpected(StoreError{"store_error", "could not initialize a signature"});
    }
    std::vector<unsigned char> signature(size);
    if (EVP_DigestSign(context.get(),
                       signature.data(),
                       &size,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return std::unexpected(StoreError{"store_error", "could not sign data"});
    }
    signature.resize(size);
    return encoded_bytes(signature);
}

Result<bool> verify(const Key& key, std::string_view message, std::string_view signature) {
    auto decoded = decoded_bytes(signature);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    std::array<unsigned char, 32> public_key{};
    std::size_t public_size = public_key.size();
    EVP_PKEY* raw_private =
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
    if (raw_private == nullptr ||
        EVP_PKEY_get_raw_public_key(raw_private, public_key.data(), &public_size) != 1 ||
        public_size != public_key.size()) {
        EVP_PKEY_free(raw_private);
        return std::unexpected(StoreError{"store_error", "could not derive a verification key"});
    }
    EVP_PKEY_free(raw_private);
    EVP_PKEY* raw_public = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size());
    if (raw_public == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not create a verification key"});
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> verification_key(raw_public, EVP_PKEY_free);
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(
            StoreError{"store_error", "could not allocate a verification context"});
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
    if (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, verification_key.get()) !=
        1) {
        return std::unexpected(StoreError{"store_error", "could not initialize verification"});
    }
    return EVP_DigestVerify(context.get(),
                            decoded->data(),
                            decoded->size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

Result<std::string> hmac(const Key& key, std::string_view message) {
    Key output{};
    unsigned int size = 0;
    const unsigned char* result = HMAC(EVP_sha256(),
                                       key.data(),
                                       static_cast<int>(key.size()),
                                       reinterpret_cast<const unsigned char*>(message.data()),
                                       message.size(),
                                       output.data(),
                                       &size);
    if (result == nullptr || size != output.size()) {
        cleanse(output);
        return std::unexpected(StoreError{"store_error", "could not calculate HMAC"});
    }
    const std::string encoded = encoded_bytes(output);
    cleanse(output);
    return encoded;
}

std::string format_ciphertext(const Ciphertext& value) {
    return "lake:v1:" + value.key_id + ":" + std::to_string(value.version) + ":" + value.sealed;
}

Result<Ciphertext> parse_ciphertext(std::string_view value) {
    constexpr std::string_view prefix = "lake:v1:";
    if (!value.starts_with(prefix)) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is not a lake v1 value"});
    }
    const std::size_t key_end = value.find(':', prefix.size());
    const std::size_t version_end =
        key_end == std::string_view::npos ? key_end : value.find(':', key_end + 1);
    if (key_end == std::string_view::npos || version_end == std::string_view::npos) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is malformed"});
    }
    Ciphertext output;
    output.key_id = std::string(value.substr(prefix.size(), key_end - prefix.size()));
    const std::string_view encoded_version = value.substr(key_end + 1, version_end - key_end - 1);
    const auto [end, error] = std::from_chars(
        encoded_version.data(), encoded_version.data() + encoded_version.size(), output.version);
    if (output.key_id.empty() || error != std::errc{} ||
        end != encoded_version.data() + encoded_version.size() || output.version == 0) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is malformed"});
    }
    output.sealed = std::string(value.substr(version_end + 1));
    if (output.sealed.empty()) {
        return std::unexpected(StoreError{"invalid_input", "ciphertext is malformed"});
    }
    return output;
}

void cleanse(Key& key) {
    OPENSSL_cleanse(key.data(), key.size());
}

} // namespace secrets_crypto

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
                  r[5].as<std::string>(),
                  r[6].as<std::string>(),
                  nlohmann::json::parse(r[7].as<std::string>())};
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

OfflineGrant offline_grant_of(const pqxx::row& row) {
    return OfflineGrant{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        row[3].as<std::string>(),
        nlohmann::json::parse(row[4].as<std::string>()).get<std::vector<std::string>>(),
        row[5].as<std::string>(),
        row[6].as<std::string>(),
        row[7].as<std::string>(),
        nlohmann::json::parse(row[8].as<std::string>())};
}

RecoveryAttempt recovery_attempt_of(const pqxx::row& row) {
    return RecoveryAttempt{row[0].as<std::string>(),
                           row[1].as<std::string>(),
                           row[2].as<std::string>(),
                           row[3].as<std::string>(),
                           row[4].as<std::string>(),
                           row[5].as<std::string>(),
                           row[6].as<std::string>(),
                           row[7].as<std::string>(),
                           row[8].as<int>()};
}

SsoProvider sso_provider_of(const pqxx::row& row) {
    return SsoProvider{row[0].as<std::string>(),
                       row[1].as<std::string>(),
                       row[2].as<std::string>(),
                       row[3].as<std::string>(),
                       row[4].as<std::string>(),
                       row[5].as<std::string>(),
                       row[6].as<std::string>(),
                       row[7].as<std::string>(),
                       row[8].as<std::string>(),
                       nlohmann::json::parse(row[9].as<std::string>()),
                       row[10].as<bool>(),
                       row[11].as<bool>(),
                       row[12].as<std::string>(),
                       row[13].as<std::string>(),
                       row[14].as<std::string>()};
}

FederationIssuer federation_issuer_of(const pqxx::row& row) {
    return FederationIssuer{row[0].as<std::string>(),
                            row[1].as<std::string>(),
                            row[2].as<std::string>(),
                            row[3].as<std::string>(),
                            row[4].as<std::string>()};
}

ScimUser scim_user_of(const pqxx::row& row) {
    return ScimUser{row[0].as<std::string>(),
                    row[1].as<std::string>(),
                    row[2].as<std::string>(),
                    row[3].as<bool>(),
                    nlohmann::json::parse(row[4].as<std::string>()),
                    row[5].as<std::string>(),
                    row[6].as<std::string>()};
}

ScimGroup scim_group_of(const pqxx::row& row) {
    return ScimGroup{row[0].as<std::string>(),
                     row[1].as<std::string>(),
                     row[2].as<std::string>(),
                     row[3].as<std::string>(),
                     nlohmann::json::parse(row[4].as<std::string>()),
                     row[5].as<std::string>(),
                     row[6].as<std::string>()};
}

ProvisioningLog provisioning_log_of(const pqxx::row& row) {
    return ProvisioningLog{row[0].as<std::string>(),
                           row[1].as<std::string>(),
                           row[2].as<std::string>(),
                           row[3].as<std::string>(),
                           row[4].as<std::string>(),
                           row[5].as<std::string>(),
                           nlohmann::json::parse(row[6].as<std::string>()),
                           row[7].as<std::string>()};
}

AgentSession agent_session_of(const pqxx::row& row) {
    return AgentSession{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        row[3].as<std::string>(),
        row[4].as<std::string>(),
        row[5].as<std::string>(),
        nlohmann::json::parse(row[6].as<std::string>()).get<std::vector<std::string>>(),
        row[7].as<std::string>(),
        row[8].as<std::uint64_t>(),
        row[9].as<std::string>(),
        row[10].as<std::int64_t>(),
        row[11].as<std::string>(),
        row[12].as<std::string>()};
}

constexpr const char* device_columns =
    "devices.id::text, devices.user_id::text, devices.name, "
    "coalesce(devices.revoked_at::text,''), devices.created_at::text, "
    "devices.updated_at::text, coalesce(passkeys.subject,''), "
    "coalesce(passkeys.public_data->'binding_attestation','null'::jsonb)::text";
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
constexpr const char* offline_grant_columns =
    "id::text, user_id::text, principal_taap_id, device_id::text, "
    "array_to_json(scopes)::text, issued_at::text, expires_at::text, "
    "coalesce(revoked_at::text,''), credential::text";
constexpr const char* recovery_attempt_columns =
    "offline_recovery_attempts.id::text, offline_recovery_attempts.user_id::text, "
    "coalesce(offline_recovery_attempts.organization_id::text,''), "
    "offline_recovery_attempts.method, offline_recovery_attempts.status, "
    "offline_recovery_attempts.ready_at::text, offline_recovery_attempts.expires_at::text, "
    "coalesce(offline_recovery_attempts.completed_at::text,''), "
    "(SELECT count(*)::integer FROM offline_recovery_admin_approvals "
    "WHERE recovery_id = offline_recovery_attempts.id)";
constexpr const char* sso_provider_columns =
    "id::text, organization_id::text, pool_id, name, protocol, issuer, metadata, "
    "signing_certificate, client_id, attribute_mapping::text, jit_provisioning, enabled, "
    "coalesce(tested_at::text,''), created_at::text, updated_at::text";
constexpr const char* federation_issuer_columns =
    "id::text, organization_id::text, issuer, claim, created_at::text";
constexpr const char* scim_user_columns =
    "provider_id::text, external_id, user_id::text, active, attributes::text, created_at::text, "
    "updated_at::text";
constexpr const char* scim_group_columns =
    "provider_id::text, external_id, team_id::text, display_name, attributes::text, "
    "created_at::text, updated_at::text";
constexpr const char* provisioning_log_columns =
    "id::text, provider_id::text, resource, operation, external_id, outcome, details::text, "
    "created_at::text";
constexpr const char* forge_record_columns =
    "id::text, repository_id::text, kind, value::text, created_at::text, updated_at::text";
constexpr const char* drive_share_link_columns =
    "id::text, repository_id::text, object_id::text, node_id, path, role, token_hash, "
    "expires_at::text, coalesce(revoked_at::text,''), created_by_taap_id, created_at::text";
constexpr const char* application_columns =
    "id::text, customer_id::text, pool_id, name, allowed_origins::text, redirect_uris::text, "
    "coalesce(custom_sign_in_domain,''), theme::text, created_at::text, updated_at::text";
constexpr const char* application_user_columns =
    "auth_app_users.application_id::text, auth_app_users.user_id::text, "
    "coalesce(users.taap_id,''), coalesce(users.email,''), users.display_name, "
    "auth_app_users.status, auth_app_users.created_at::text, auth_app_users.updated_at::text";
constexpr const char* application_organization_columns =
    "id::text, application_id::text, taap_id, name, metadata::text, created_at::text, "
    "updated_at::text";
constexpr const char* application_webhook_columns =
    "id::text, application_id::text, url, array_to_json(events)::text, encrypted_secret, "
    "enabled, created_at::text, updated_at::text";
constexpr const char* application_webhook_event_columns =
    "id::text, application_id::text, event, payload::text, created_at::text";
constexpr const char* application_webhook_delivery_columns =
    "id::text, event_id::text, webhook_id::text, attempt, status, signature, error, "
    "created_at::text";

Application application_of(const pqxx::row& row) {
    return Application{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        row[3].as<std::string>(),
        nlohmann::json::parse(row[4].as<std::string>()).get<std::vector<std::string>>(),
        nlohmann::json::parse(row[5].as<std::string>()).get<std::vector<std::string>>(),
        row[6].as<std::string>(),
        nlohmann::json::parse(row[7].as<std::string>()),
        row[8].as<std::string>(),
        row[9].as<std::string>()};
}

ApplicationUser application_user_of(const pqxx::row& row) {
    return ApplicationUser{row[0].as<std::string>(),
                           row[1].as<std::string>(),
                           row[2].as<std::string>(),
                           row[3].as<std::string>(),
                           row[4].as<std::string>(),
                           row[5].as<std::string>(),
                           row[6].as<std::string>(),
                           row[7].as<std::string>()};
}

ApplicationOrganization application_organization_of(const pqxx::row& row) {
    return ApplicationOrganization{row[0].as<std::string>(),
                                   row[1].as<std::string>(),
                                   row[2].as<std::string>(),
                                   row[3].as<std::string>(),
                                   nlohmann::json::parse(row[4].as<std::string>()),
                                   row[5].as<std::string>(),
                                   row[6].as<std::string>()};
}

ApplicationWebhook application_webhook_of(const pqxx::row& row, std::string secret) {
    return ApplicationWebhook{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        nlohmann::json::parse(row[3].as<std::string>()).get<std::vector<std::string>>(),
        std::move(secret),
        row[5].as<bool>(),
        row[6].as<std::string>(),
        row[7].as<std::string>()};
}

ApplicationWebhookEvent application_webhook_event_of(const pqxx::row& row) {
    return ApplicationWebhookEvent{row[0].as<std::string>(),
                                   row[1].as<std::string>(),
                                   row[2].as<std::string>(),
                                   nlohmann::json::parse(row[3].as<std::string>()),
                                   row[4].as<std::string>()};
}

ApplicationWebhookDelivery application_webhook_delivery_of(const pqxx::row& row) {
    return ApplicationWebhookDelivery{row[0].as<std::string>(),
                                      row[1].as<std::string>(),
                                      row[2].as<std::string>(),
                                      row[3].as<int>(),
                                      row[4].as<int>(),
                                      row[5].as<std::string>(),
                                      row[6].as<std::string>(),
                                      row[7].as<std::string>()};
}

ForgeRecord forge_record_of(const pqxx::row& row) {
    return ForgeRecord{row[0].as<std::string>(),
                       row[1].as<std::string>(),
                       row[2].as<std::string>(),
                       row[4].as<std::string>(),
                       row[5].as<std::string>(),
                       nlohmann::json::parse(row[3].as<std::string>())};
}

DriveShareLink drive_share_link_of(const pqxx::row& row) {
    return DriveShareLink{row[0].as<std::string>(),
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

void remove_scim_user_access(pqxx::work& tx,
                             const std::string& provider_id,
                             const std::string& external_id,
                             const std::string& user_id) {
    tx.exec_params("UPDATE sessions SET revoked_at = now() WHERE user_id = $1::uuid "
                   "AND revoked_at IS NULL",
                   user_id);
    tx.exec_params(
        "DELETE FROM grants USING scim_group_members, memberships, objects WHERE "
        "scim_group_members.membership_id = memberships.id AND objects.object_type = 'team' "
        "AND objects.external_id = memberships.team_id::text AND grants.object_id = objects.id "
        "AND grants.principal_taap_id = memberships.principal_taap_id "
        "AND grants.attestation_digest = memberships.attestation_digest "
        "AND scim_group_members.provider_id = $1::uuid "
        "AND scim_group_members.user_external_id = $2",
        provider_id,
        external_id);
    tx.exec_params("DELETE FROM memberships USING scim_group_members WHERE memberships.id = "
                   "scim_group_members.membership_id AND scim_group_members.provider_id = $1::uuid "
                   "AND scim_group_members.user_external_id = $2",
                   provider_id,
                   external_id);
}

constexpr const char* secret_mount_columns =
    "id::text, customer_id::text, organization_id::text, name, description, "
    "created_at::text, updated_at::text";
constexpr const char* encryption_key_columns =
    "id::text, mount_id::text, name, coalesce(derived_from_id::text,''), "
    "current_version, created_at::text, updated_at::text";

SecretMount secret_mount_of(const pqxx::row& row) {
    return SecretMount{row[0].as<std::string>(),
                       row[1].as<std::string>(),
                       row[2].as<std::string>(),
                       row[3].as<std::string>(),
                       row[4].as<std::string>(),
                       row[5].as<std::string>(),
                       row[6].as<std::string>()};
}

EncryptionKey encryption_key_of(const pqxx::row& row) {
    return EncryptionKey{row[0].as<std::string>(),
                         row[1].as<std::string>(),
                         row[2].as<std::string>(),
                         row[3].as<std::string>(),
                         row[4].as<std::uint64_t>(),
                         row[5].as<std::string>(),
                         row[6].as<std::string>()};
}

class PostgresStore final : public Store, public PlatformStore {
public:
    PostgresStore(std::string uri, std::string instance_kek)
        : uri_(std::move(uri)), instance_kek_(std::move(instance_kek)) {}
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

    Result<PlatformEnvironment> ensure_platform_environment(const std::string& repository_id,
                                                            const std::string& name) override {
        if (repository_id.empty() || name.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "repository_id and environment name are required"});
        }
        return with_tx<PlatformEnvironment>([&](pqxx::work& tx) -> Result<PlatformEnvironment> {
            const pqxx::result existing = tx.exec_params(
                "SELECT id::text, repository_id::text, name, object_id::text, created_at::text "
                "FROM platform_environments WHERE repository_id = $1::uuid AND name = $2",
                repository_id,
                name);
            if (!existing.empty()) {
                return PlatformEnvironment{existing[0][0].as<std::string>(),
                                           existing[0][1].as<std::string>(),
                                           existing[0][2].as<std::string>(),
                                           existing[0][3].as<std::string>(),
                                           existing[0][4].as<std::string>()};
            }
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = 'repository' AND "
                               "external_id = $1",
                               repository_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "repository policy object is missing"});
            }
            const std::string environment_id = new_uuid_v4();
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('environment', $1, $2::uuid) RETURNING id::text",
                               environment_id,
                               parents[0][0].as<std::string>());
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO platform_environments (id, repository_id, name, object_id) "
                "VALUES ($1::uuid, $2::uuid, $3, $4::uuid) "
                "RETURNING id::text, repository_id::text, name, object_id::text, created_at::text",
                environment_id,
                repository_id,
                name,
                objects[0][0].as<std::string>());
            return PlatformEnvironment{rows[0][0].as<std::string>(),
                                       rows[0][1].as<std::string>(),
                                       rows[0][2].as<std::string>(),
                                       rows[0][3].as<std::string>(),
                                       rows[0][4].as<std::string>()};
        });
    }

    Result<PlatformEnvironment> get_platform_environment(const std::string& repository_id,
                                                         const std::string& name) override {
        return with_tx<PlatformEnvironment>([&](pqxx::work& tx) -> Result<PlatformEnvironment> {
            const pqxx::result rows = tx.exec_params(
                "SELECT id::text, repository_id::text, name, object_id::text, created_at::text "
                "FROM platform_environments WHERE repository_id = $1::uuid AND name = $2",
                repository_id,
                name);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no environment " + name});
            }
            return PlatformEnvironment{rows[0][0].as<std::string>(),
                                       rows[0][1].as<std::string>(),
                                       rows[0][2].as<std::string>(),
                                       rows[0][3].as<std::string>(),
                                       rows[0][4].as<std::string>()};
        });
    }

    Result<std::vector<PlatformEnvironment>>
    list_platform_environments(const std::string& repository_id) override {
        return with_tx<std::vector<PlatformEnvironment>>(
            [&](pqxx::work& tx) -> Result<std::vector<PlatformEnvironment>> {
                pqxx::result rows;
                if (repository_id.empty()) {
                    rows = tx.exec("SELECT id::text, repository_id::text, name, object_id::text, "
                                   "created_at::text FROM platform_environments ORDER BY name");
                } else {
                    rows = tx.exec_params(
                        "SELECT id::text, repository_id::text, name, object_id::text, "
                        "created_at::text FROM platform_environments "
                        "WHERE repository_id = $1::uuid ORDER BY name",
                        repository_id);
                }
                std::vector<PlatformEnvironment> output;
                for (const pqxx::row& row : rows) {
                    output.push_back(PlatformEnvironment{row[0].as<std::string>(),
                                                         row[1].as<std::string>(),
                                                         row[2].as<std::string>(),
                                                         row[3].as<std::string>(),
                                                         row[4].as<std::string>()});
                }
                return output;
            });
    }

    Result<void> upsert_platform_mirror(const PlatformMirrorRow& row) override {
        if (row.kind.empty() || row.key.empty() || !row.value.is_object()) {
            return std::unexpected(
                StoreError{"invalid_input", "platform mirror row is incomplete"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("INSERT INTO platform_mirror (kind, key, value, seen_at) "
                           "VALUES ($1, $2, $3::jsonb, $4::timestamptz) "
                           "ON CONFLICT (kind, key) DO UPDATE SET value = EXCLUDED.value, "
                           "seen_at = EXCLUDED.seen_at",
                           row.kind,
                           row.key,
                           row.value.dump(),
                           row.seen_at);
            return {};
        });
    }

    Result<std::vector<PlatformMirrorRow>> list_platform_mirror(const std::string& kind) override {
        return with_tx<std::vector<PlatformMirrorRow>>(
            [&](pqxx::work& tx) -> Result<std::vector<PlatformMirrorRow>> {
                pqxx::result rows;
                if (kind.empty()) {
                    rows = tx.exec("SELECT kind, key, value::text, seen_at::text "
                                   "FROM platform_mirror ORDER BY kind, key");
                } else {
                    rows = tx.exec_params(
                        "SELECT kind, key, value::text, seen_at::text FROM platform_mirror "
                        "WHERE kind = $1 ORDER BY key",
                        kind);
                }
                std::vector<PlatformMirrorRow> output;
                for (const pqxx::row& row : rows) {
                    const nlohmann::json value =
                        nlohmann::json::parse(row[2].as<std::string>(), nullptr, false);
                    if (value.is_discarded() || !value.is_object()) {
                        return std::unexpected(
                            StoreError{"store_error", "platform mirror contains invalid JSON"});
                    }
                    output.push_back(PlatformMirrorRow{row[0].as<std::string>(),
                                                       row[1].as<std::string>(),
                                                       value,
                                                       row[3].as<std::string>()});
                }
                return output;
            });
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

    Result<ForgeRecord> create_forge_record(const NewForgeRecord& input) override {
        return with_tx<ForgeRecord>([&](pqxx::work& tx) -> Result<ForgeRecord> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO forge_records (repository_id, kind, value) VALUES "
                            "($1::uuid, $2, $3::jsonb) RETURNING ") +
                    forge_record_columns,
                input.repository_id,
                input.kind,
                input.value.dump());
            return forge_record_of(rows[0]);
        });
    }

    Result<ForgeRecord> get_forge_record(const std::string& id) override {
        return with_tx<ForgeRecord>([&](pqxx::work& tx) -> Result<ForgeRecord> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + forge_record_columns +
                                                         " FROM forge_records WHERE id = $1::uuid",
                                                     id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no forge record " + id});
            }
            return forge_record_of(rows[0]);
        });
    }

    Result<std::vector<ForgeRecord>> list_forge_records(const std::string& repository_id,
                                                        const std::string& kind,
                                                        int limit) override {
        return with_tx<std::vector<ForgeRecord>>(
            [&](pqxx::work& tx) -> Result<std::vector<ForgeRecord>> {
                const pqxx::result rows =
                    kind.empty() ? tx.exec_params(std::string("SELECT ") + forge_record_columns +
                                                      " FROM forge_records WHERE "
                                                      "repository_id = $1::uuid "
                                                      "ORDER BY created_at DESC LIMIT $2",
                                                  repository_id,
                                                  limit)
                                 : tx.exec_params(std::string("SELECT ") + forge_record_columns +
                                                      " FROM forge_records WHERE "
                                                      "repository_id = $1::uuid AND "
                                                      "kind = $2 ORDER BY created_at "
                                                      "DESC LIMIT $3",
                                                  repository_id,
                                                  kind,
                                                  limit);
                std::vector<ForgeRecord> output;
                for (const auto& row : rows) {
                    output.push_back(forge_record_of(row));
                }
                return output;
            });
    }

    Result<ForgeRecord> update_forge_record(const std::string& id,
                                            const nlohmann::json& value) override {
        return with_tx<ForgeRecord>([&](pqxx::work& tx) -> Result<ForgeRecord> {
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE forge_records SET value = $2::jsonb WHERE id = $1::uuid "
                            "RETURNING ") +
                    forge_record_columns,
                id,
                value.dump());
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no forge record " + id});
            }
            return forge_record_of(rows[0]);
        });
    }

    Result<DriveShareLink> create_drive_share_link(const NewDriveShareLink& input) override {
        return with_tx<DriveShareLink>([&](pqxx::work& tx) -> Result<DriveShareLink> {
            const pqxx::result object = tx.exec_params(
                "SELECT object_type FROM objects WHERE id = $1::uuid", input.object_id);
            if (object.empty() || object[0][0].as<std::string>() != "path") {
                return std::unexpected(
                    StoreError{"invalid_input", "object_id does not reference a drive folder"});
            }
            if (input.role != "read" && input.role != "write") {
                return std::unexpected(
                    StoreError{"invalid_input", "share link role must be read or write"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO drive_share_links (repository_id, object_id, node_id, "
                            "path, role, token_hash, expires_at, created_by_taap_id) VALUES "
                            "($1::uuid, $2::uuid, $3, $4, $5, $6, $7::timestamptz, $8) "
                            "RETURNING ") +
                    drive_share_link_columns,
                input.repository_id,
                input.object_id,
                input.node_id,
                input.path,
                input.role,
                input.token_hash,
                input.expires_at,
                input.created_by_taap_id);
            const DriveShareLink link = drive_share_link_of(rows[0]);
            tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                           "attestation_digest, granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, "
                           "$5)",
                           "share-link:" + link.id,
                           link.object_id,
                           link.role,
                           "drive_share_link/" + link.id,
                           input.created_by_taap_id);
            return link;
        });
    }

    Result<DriveShareLink> get_drive_share_link(const std::string& id) override {
        return with_tx<DriveShareLink>([&](pqxx::work& tx) -> Result<DriveShareLink> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + drive_share_link_columns +
                                   " FROM drive_share_links WHERE id = $1::uuid",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no drive share link " + id});
            }
            return drive_share_link_of(rows[0]);
        });
    }

    Result<DriveShareLink>
    get_drive_share_link_by_token_hash(const std::string& token_hash) override {
        return with_tx<DriveShareLink>([&](pqxx::work& tx) -> Result<DriveShareLink> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + drive_share_link_columns +
                                   " FROM drive_share_links WHERE token_hash = $1",
                               token_hash);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "share link is not recognized"});
            }
            return drive_share_link_of(rows[0]);
        });
    }

    Result<std::vector<DriveShareLink>> list_drive_share_links(const std::string& repository_id,
                                                               const std::string& object_id,
                                                               int limit) override {
        return with_tx<std::vector<DriveShareLink>>(
            [&](pqxx::work& tx) -> Result<std::vector<DriveShareLink>> {
                const pqxx::result rows =
                    object_id.empty()
                        ? tx.exec_params(std::string("SELECT ") + drive_share_link_columns +
                                             " FROM drive_share_links WHERE repository_id = "
                                             "$1::uuid ORDER BY created_at DESC LIMIT $2",
                                         repository_id,
                                         limit)
                        : tx.exec_params(std::string("SELECT ") + drive_share_link_columns +
                                             " FROM drive_share_links WHERE repository_id = "
                                             "$1::uuid AND object_id = $2::uuid ORDER BY "
                                             "created_at DESC LIMIT $3",
                                         repository_id,
                                         object_id,
                                         limit);
                std::vector<DriveShareLink> output;
                for (const auto& row : rows) {
                    output.push_back(drive_share_link_of(row));
                }
                return output;
            });
    }

    Result<DriveShareLink> revoke_drive_share_link(const std::string& id) override {
        return with_tx<DriveShareLink>([&](pqxx::work& tx) -> Result<DriveShareLink> {
            const pqxx::result current =
                tx.exec_params(std::string("SELECT ") + drive_share_link_columns +
                                   " FROM drive_share_links WHERE id = $1::uuid FOR UPDATE",
                               id);
            if (current.empty()) {
                return std::unexpected(StoreError{"not_found", "no drive share link " + id});
            }
            if (!drive_share_link_of(current[0]).revoked_at.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "drive share link is already revoked"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE drive_share_links SET revoked_at = now() WHERE id = $1::uuid "
                            "RETURNING ") +
                    drive_share_link_columns,
                id);
            return drive_share_link_of(rows[0]);
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

    Result<Membership> get_membership(const std::string& id) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + membership_columns +
                                                         " FROM memberships WHERE id = $1::uuid",
                                                     id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            return membership_of(rows[0]);
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

    Result<Membership> set_member_role(const std::string& id,
                                       const std::string& role,
                                       const std::string& actor) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            if (role != "owner" && role != "admin" && role != "member") {
                return std::unexpected(
                    StoreError{"invalid_input", "role must be owner, admin, or member"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + membership_columns +
                                   " FROM memberships WHERE id = $1::uuid FOR UPDATE",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            const Membership membership = membership_of(rows[0]);
            if (membership.role == "owner" && role != "owner") {
                const pqxx::result owners = tx.exec_params(
                    "SELECT count(*) FROM memberships WHERE organization_id = $1::uuid "
                    "AND team_id IS NOT DISTINCT FROM $2::uuid AND role = 'owner'",
                    membership.organization_id,
                    nullable(membership.team_id));
                if (owners[0][0].as<int>() == 1) {
                    return std::unexpected(StoreError{
                        "conflict", "cannot demote the last owner in this membership scope"});
                }
            }
            const std::string object_type = membership.team_id.empty() ? "organization" : "team";
            const std::string external_id =
                membership.team_id.empty() ? membership.organization_id : membership.team_id;
            const pqxx::result objects = tx.exec_params(
                "SELECT id::text FROM objects WHERE object_type = $1 AND external_id = $2",
                object_type,
                external_id);
            if (objects.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "membership policy parent is missing"});
            }
            const std::string previous_grant_role = membership.role == "member" ? "read" : "admin";
            const std::string next_grant_role = role == "member" ? "read" : "admin";
            const pqxx::result removed = tx.exec_params(
                "DELETE FROM grants WHERE principal_taap_id = $1 AND object_id = $2::uuid "
                "AND role = $3 AND attestation_digest = $4 AND granted_by_taap_id = $5",
                membership.principal_taap_id,
                objects[0][0].as<std::string>(),
                previous_grant_role,
                membership.attestation_digest,
                membership.granted_by_taap_id);
            if (removed.affected_rows() != 1) {
                return std::unexpected(
                    StoreError{"store_error", "membership derived grant is missing"});
            }
            tx.exec_params(
                "INSERT INTO grants (principal_taap_id, object_id, role, attestation_digest, "
                "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5) ON CONFLICT DO NOTHING",
                membership.principal_taap_id,
                objects[0][0].as<std::string>(),
                next_grant_role,
                membership.attestation_digest,
                membership.granted_by_taap_id);
            const pqxx::result updated = tx.exec_params(
                std::string("UPDATE memberships SET role = $2 WHERE id = $1::uuid RETURNING ") +
                    membership_columns,
                id,
                role);
            static_cast<void>(actor);
            return membership_of(updated[0]);
        });
    }

    Result<void> remove_member(const std::string& id, const std::string& actor) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + membership_columns +
                                   " FROM memberships WHERE id = $1::uuid FOR UPDATE",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            const Membership membership = membership_of(rows[0]);
            if (membership.role == "owner") {
                const pqxx::result owners = tx.exec_params(
                    "SELECT count(*) FROM memberships WHERE organization_id = $1::uuid "
                    "AND team_id IS NOT DISTINCT FROM $2::uuid AND role = 'owner'",
                    membership.organization_id,
                    nullable(membership.team_id));
                if (owners[0][0].as<int>() == 1) {
                    const std::string message =
                        membership.principal_taap_id == actor
                            ? "cannot remove yourself as the last owner in this membership scope"
                            : "cannot remove the last owner in this membership scope";
                    return std::unexpected(StoreError{"conflict", message});
                }
            }
            const std::string object_type = membership.team_id.empty() ? "organization" : "team";
            const std::string external_id =
                membership.team_id.empty() ? membership.organization_id : membership.team_id;
            const pqxx::result objects = tx.exec_params(
                "SELECT id::text FROM objects WHERE object_type = $1 AND external_id = $2",
                object_type,
                external_id);
            if (objects.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "membership policy parent is missing"});
            }
            tx.exec_params(
                "DELETE FROM grants WHERE principal_taap_id = $1 AND object_id = $2::uuid "
                "AND role = $3 AND attestation_digest = $4 AND granted_by_taap_id = $5",
                membership.principal_taap_id,
                objects[0][0].as<std::string>(),
                membership.role == "member" ? "read" : "admin",
                membership.attestation_digest,
                membership.granted_by_taap_id);
            tx.exec_params("DELETE FROM objects WHERE object_type = 'member' AND external_id = $1",
                           id);
            tx.exec_params("DELETE FROM memberships WHERE id = $1::uuid", id);
            return {};
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

    Result<Application> create_application(const NewApplication& input) override {
        return with_tx<Application>([&](pqxx::work& tx) -> Result<Application> {
            const pqxx::result parents = tx.exec_params(
                "SELECT id::text FROM objects WHERE object_type = 'customer' AND external_id = $1",
                input.customer_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "application customer policy object is missing"});
            }
            const std::string purpose = "application.jwt." + input.id;
            const auto encrypted_seed = encrypt_secret(tx, purpose, input.signing_key.seed);
            if (!encrypted_seed) {
                return std::unexpected(encrypted_seed.error());
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO auth_applications (id, customer_id, pool_id, name, "
                            "allowed_origins, redirect_uris, custom_sign_in_domain, theme) VALUES "
                            "($1::uuid, $2::uuid, $3, $4, $5::jsonb, $6::jsonb, NULLIF($7, ''), "
                            "$8::jsonb) RETURNING ") +
                    application_columns,
                input.id,
                input.customer_id,
                input.pool_id,
                input.name,
                nlohmann::json(input.allowed_origins).dump(),
                nlohmann::json(input.redirect_uris).dump(),
                input.custom_sign_in_domain,
                input.theme.dump());
            tx.exec_params("INSERT INTO auth_app_signing_keys (application_id, kid, version, "
                           "encrypted_seed, public_key, current) VALUES "
                           "($1::uuid, $2, $3, $4, $5, true)",
                           input.id,
                           input.signing_key.kid,
                           input.signing_key.version,
                           *encrypted_seed,
                           input.signing_key.public_key);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) VALUES "
                           "('application', $1, $2::uuid)",
                           input.id,
                           parents[0][0].as<std::string>());
            return application_of(rows[0]);
        });
    }

    Result<std::vector<Application>> list_applications(const std::string& customer_id,
                                                       int limit) override {
        return with_tx<std::vector<Application>>(
            [&](pqxx::work& tx) -> Result<std::vector<Application>> {
                const pqxx::result rows =
                    customer_id.empty()
                        ? tx.exec_params(std::string("SELECT ") + application_columns +
                                             " FROM auth_applications ORDER BY "
                                             "created_at DESC LIMIT $1",
                                         limit)
                        : tx.exec_params(std::string("SELECT ") + application_columns +
                                             " FROM auth_applications WHERE "
                                             "customer_id = $1::uuid ORDER BY "
                                             "created_at DESC LIMIT $2",
                                         customer_id,
                                         limit);
                std::vector<Application> output;
                for (const auto& row : rows) {
                    output.push_back(application_of(row));
                }
                return output;
            });
    }

    Result<Application> get_application(const std::string& id) override {
        return application_by("id = $1::uuid", id);
    }

    Result<Application> get_application_by_pool(const std::string& pool_id) override {
        return application_by("pool_id = $1", pool_id);
    }

    Result<Application>
    get_application_by_domain(const std::string& custom_sign_in_domain) override {
        return application_by("lower(custom_sign_in_domain) = lower($1)", custom_sign_in_domain);
    }

    Result<Application> update_application_theme(const std::string& id,
                                                 const nlohmann::json& theme) override {
        return with_tx<Application>([&](pqxx::work& tx) -> Result<Application> {
            if (!theme.is_object()) {
                return std::unexpected(
                    StoreError{"invalid_input", "application theme must be an object"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE auth_applications SET theme = $2::jsonb WHERE id = $1::uuid "
                            "RETURNING ") +
                    application_columns,
                id,
                theme.dump());
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no application " + id});
            }
            return application_of(rows[0]);
        });
    }

    Result<ApplicationSigningKey>
    rotate_application_signing_key(const ApplicationSigningKey& input) override {
        return with_tx<ApplicationSigningKey>([&](pqxx::work& tx) -> Result<ApplicationSigningKey> {
            const std::string purpose = "application.jwt." + input.application_id;
            const auto encrypted_seed = encrypt_secret(tx, purpose, input.seed);
            if (!encrypted_seed) {
                return std::unexpected(encrypted_seed.error());
            }
            tx.exec_params("UPDATE auth_app_signing_keys SET current = false, retired_at = "
                           "now() WHERE application_id = $1::uuid AND current",
                           input.application_id);
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO auth_app_signing_keys (application_id, kid, version, "
                "encrypted_seed, public_key, current) VALUES ($1::uuid, $2, $3, $4, $5, true) "
                "RETURNING created_at::text",
                input.application_id,
                input.kid,
                input.version,
                *encrypted_seed,
                input.public_key);
            ApplicationSigningKey output = input;
            output.current = true;
            output.created_at = rows[0][0].as<std::string>();
            return output;
        });
    }

    Result<ApplicationSigningKey>
    current_application_signing_key(const std::string& application_id) override {
        return with_tx<ApplicationSigningKey>([&](pqxx::work& tx) -> Result<ApplicationSigningKey> {
            const pqxx::result rows =
                tx.exec_params("SELECT kid, version, encrypted_seed, public_key, created_at::text, "
                               "coalesce(retired_at::text,'') FROM auth_app_signing_keys WHERE "
                               "application_id = $1::uuid AND current",
                               application_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "application has no current signing key"});
            }
            const auto seed = decrypt_secret(
                tx, "application.jwt." + application_id, rows[0][2].as<std::string>());
            if (!seed) {
                return std::unexpected(seed.error());
            }
            return ApplicationSigningKey{application_id,
                                         rows[0][0].as<std::string>(),
                                         *seed,
                                         rows[0][3].as<std::string>(),
                                         rows[0][1].as<int>(),
                                         true,
                                         rows[0][4].as<std::string>(),
                                         rows[0][5].as<std::string>()};
        });
    }

    Result<std::vector<ApplicationSigningKey>>
    list_application_signing_keys(const std::string& application_id) override {
        return with_tx<std::vector<ApplicationSigningKey>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationSigningKey>> {
                const pqxx::result rows =
                    tx.exec_params("SELECT kid, version, public_key, current, created_at::text, "
                                   "coalesce(retired_at::text,'') FROM auth_app_signing_keys WHERE "
                                   "application_id = $1::uuid ORDER BY version",
                                   application_id);
                std::vector<ApplicationSigningKey> output;
                for (const auto& row : rows) {
                    output.push_back(ApplicationSigningKey{application_id,
                                                           row[0].as<std::string>(),
                                                           {},
                                                           row[2].as<std::string>(),
                                                           row[1].as<int>(),
                                                           row[3].as<bool>(),
                                                           row[4].as<std::string>(),
                                                           row[5].as<std::string>()});
                }
                return output;
            });
    }

    Result<ApplicationUser> register_application_user(const std::string& application_id,
                                                      const std::string& user_id) override {
        return with_tx<ApplicationUser>([&](pqxx::work& tx) -> Result<ApplicationUser> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO auth_app_users (application_id, user_id) VALUES "
                            "($1::uuid, $2::uuid) ON CONFLICT (application_id, user_id) DO UPDATE "
                            "SET updated_at = auth_app_users.updated_at RETURNING user_id::text"),
                application_id,
                user_id);
            static_cast<void>(rows);
            return application_user_tx(tx, application_id, user_id);
        });
    }

    Result<ApplicationUser> get_application_user(const std::string& application_id,
                                                 const std::string& user_id) override {
        return with_tx<ApplicationUser>(
            [&](pqxx::work& tx) { return application_user_tx(tx, application_id, user_id); });
    }

    Result<std::vector<ApplicationUser>> list_application_users(const std::string& application_id,
                                                                const std::string& query,
                                                                int limit) override {
        return with_tx<std::vector<ApplicationUser>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationUser>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + application_user_columns +
                        " FROM auth_app_users JOIN users ON users.id = auth_app_users.user_id "
                        "WHERE auth_app_users.application_id = $1::uuid AND ($2 = '' OR "
                        "coalesce(users.email,'') ILIKE '%' || $2 || '%' OR users.display_name "
                        "ILIKE '%' || $2 || '%') ORDER BY auth_app_users.created_at DESC LIMIT $3",
                    application_id,
                    query,
                    limit);
                std::vector<ApplicationUser> output;
                for (const auto& row : rows) {
                    output.push_back(application_user_of(row));
                }
                return output;
            });
    }

    Result<ApplicationUser> set_application_user_status(const std::string& application_id,
                                                        const std::string& user_id,
                                                        const std::string& status) override {
        return with_tx<ApplicationUser>([&](pqxx::work& tx) -> Result<ApplicationUser> {
            if (status != "active" && status != "suspended") {
                return std::unexpected(
                    StoreError{"invalid_input", "application user status is invalid"});
            }
            const pqxx::result updated = tx.exec_params(
                "UPDATE auth_app_users SET status = $3 WHERE application_id = $1::uuid AND "
                "user_id = $2::uuid RETURNING user_id::text",
                application_id,
                user_id,
                status);
            if (updated.empty()) {
                return std::unexpected(StoreError{"not_found", "application user does not exist"});
            }
            if (status == "suspended") {
                tx.exec_params("UPDATE sessions SET revoked_at = now() FROM auth_applications "
                               "WHERE auth_applications.id = $1::uuid AND sessions.user_id = "
                               "$2::uuid AND sessions.pool_id = auth_applications.pool_id AND "
                               "sessions.revoked_at IS NULL",
                               application_id,
                               user_id);
            }
            return application_user_tx(tx, application_id, user_id);
        });
    }

    Result<void> delete_application_user(const std::string& application_id,
                                         const std::string& user_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result applications = tx.exec_params(
                "SELECT pool_id FROM auth_applications WHERE id = $1::uuid", application_id);
            if (applications.empty()) {
                return std::unexpected(StoreError{"not_found", "application does not exist"});
            }
            const std::string pool_id = applications[0][0].as<std::string>();
            tx.exec_params(
                "DELETE FROM sessions WHERE user_id = $1::uuid AND pool_id = $2", user_id, pool_id);
            tx.exec_params("DELETE FROM credentials WHERE user_id = $1::uuid AND pool_id = $2",
                           user_id,
                           pool_id);
            const pqxx::result removed = tx.exec_params(
                "DELETE FROM auth_app_users WHERE application_id = $1::uuid AND user_id = "
                "$2::uuid RETURNING user_id::text",
                application_id,
                user_id);
            if (removed.empty()) {
                return std::unexpected(StoreError{"not_found", "application user does not exist"});
            }
            return {};
        });
    }

    Result<ApplicationOrganization>
    create_application_organization(const NewApplicationOrganization& input) override {
        return with_tx<ApplicationOrganization>(
            [&](pqxx::work& tx) -> Result<ApplicationOrganization> {
                const pqxx::result rows = tx.exec_params(
                    std::string("INSERT INTO auth_app_organizations (application_id, taap_id, "
                                "name, metadata) VALUES ($1::uuid, $2, $3, $4::jsonb) RETURNING ") +
                        application_organization_columns,
                    input.application_id,
                    input.taap_id,
                    input.name,
                    input.metadata.dump());
                return application_organization_of(rows[0]);
            });
    }

    Result<std::vector<ApplicationOrganization>>
    list_application_organizations(const std::string& application_id, int limit) override {
        return with_tx<std::vector<ApplicationOrganization>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationOrganization>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + application_organization_columns +
                        " FROM auth_app_organizations WHERE application_id = $1::uuid ORDER BY "
                        "created_at DESC LIMIT $2",
                    application_id,
                    limit);
                std::vector<ApplicationOrganization> output;
                for (const auto& row : rows) {
                    output.push_back(application_organization_of(row));
                }
                return output;
            });
    }

    Result<ApplicationOrganization> update_application_organization(
        const std::string& id, const std::string& name, const nlohmann::json& metadata) override {
        return with_tx<ApplicationOrganization>(
            [&](pqxx::work& tx) -> Result<ApplicationOrganization> {
                const pqxx::result rows = tx.exec_params(
                    std::string("UPDATE auth_app_organizations SET name = CASE WHEN $2 = '' THEN "
                                "name ELSE $2 END, metadata = $3::jsonb WHERE id = $1::uuid "
                                "RETURNING ") +
                        application_organization_columns,
                    id,
                    name,
                    metadata.dump());
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "application organization does not exist"});
                }
                return application_organization_of(rows[0]);
            });
    }

    Result<void> delete_application_organization(const std::string& id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows = tx.exec_params(
                "DELETE FROM auth_app_organizations WHERE id = $1::uuid RETURNING id::text", id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "application organization does not exist"});
            }
            return {};
        });
    }

    Result<ApplicationWebhook>
    create_application_webhook(const NewApplicationWebhook& input) override {
        return with_tx<ApplicationWebhook>([&](pqxx::work& tx) -> Result<ApplicationWebhook> {
            const std::string id = new_uuid_v4();
            const auto encrypted = encrypt_secret(tx, "application.webhook." + id, input.secret);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            const pqxx::result rows = tx.exec_params(
                std::string(
                    "INSERT INTO auth_app_webhooks (id, application_id, url, events, "
                    "encrypted_secret) VALUES ($1::uuid, $2::uuid, $3, $4, $5) RETURNING ") +
                    application_webhook_columns,
                id,
                input.application_id,
                input.url,
                input.events,
                *encrypted);
            return application_webhook_of(rows[0], input.secret);
        });
    }

    Result<ApplicationWebhook> get_application_webhook(const std::string& id) override {
        return with_tx<ApplicationWebhook>([&](pqxx::work& tx) -> Result<ApplicationWebhook> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + application_webhook_columns +
                                   " FROM auth_app_webhooks WHERE id = $1::uuid",
                               id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "application webhook does not exist"});
            }
            const auto secret =
                decrypt_secret(tx, "application.webhook." + id, rows[0][4].as<std::string>());
            if (!secret) {
                return std::unexpected(secret.error());
            }
            return application_webhook_of(rows[0], *secret);
        });
    }

    Result<std::vector<ApplicationWebhook>>
    list_application_webhooks(const std::string& application_id, int limit) override {
        return with_tx<std::vector<ApplicationWebhook>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationWebhook>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + application_webhook_columns +
                        " FROM auth_app_webhooks WHERE application_id = $1::uuid ORDER BY "
                        "created_at DESC LIMIT $2",
                    application_id,
                    limit);
                std::vector<ApplicationWebhook> output;
                for (const auto& row : rows) {
                    const std::string id = row[0].as<std::string>();
                    const auto secret =
                        decrypt_secret(tx, "application.webhook." + id, row[4].as<std::string>());
                    if (!secret) {
                        return std::unexpected(secret.error());
                    }
                    output.push_back(application_webhook_of(row, *secret));
                }
                return output;
            });
    }

    Result<ApplicationWebhookEvent>
    create_application_webhook_event(const std::string& application_id,
                                     const std::string& event,
                                     const nlohmann::json& payload) override {
        return with_tx<ApplicationWebhookEvent>(
            [&](pqxx::work& tx) -> Result<ApplicationWebhookEvent> {
                const pqxx::result rows = tx.exec_params(
                    std::string("INSERT INTO auth_app_webhook_events (application_id, event, "
                                "payload) VALUES ($1::uuid, $2, $3::jsonb) RETURNING ") +
                        application_webhook_event_columns,
                    application_id,
                    event,
                    payload.dump());
                return application_webhook_event_of(rows[0]);
            });
    }

    Result<ApplicationWebhookEvent> get_application_webhook_event(const std::string& id) override {
        return with_tx<ApplicationWebhookEvent>(
            [&](pqxx::work& tx) -> Result<ApplicationWebhookEvent> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + application_webhook_event_columns +
                                       " FROM auth_app_webhook_events WHERE id = $1::uuid",
                                   id);
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "application webhook event does not exist"});
                }
                return application_webhook_event_of(rows[0]);
            });
    }

    Result<std::vector<ApplicationWebhookEvent>>
    list_application_webhook_events(const std::string& application_id, int limit) override {
        return with_tx<std::vector<ApplicationWebhookEvent>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationWebhookEvent>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + application_webhook_event_columns +
                        " FROM auth_app_webhook_events WHERE application_id = $1::uuid ORDER BY "
                        "created_at DESC LIMIT $2",
                    application_id,
                    limit);
                std::vector<ApplicationWebhookEvent> output;
                for (const auto& row : rows) {
                    output.push_back(application_webhook_event_of(row));
                }
                return output;
            });
    }

    Result<ApplicationWebhookDelivery>
    create_application_webhook_delivery(const NewApplicationWebhookDelivery& input) override {
        return with_tx<ApplicationWebhookDelivery>(
            [&](pqxx::work& tx) -> Result<ApplicationWebhookDelivery> {
                const pqxx::result rows = tx.exec_params(
                    std::string("INSERT INTO auth_app_webhook_deliveries (event_id, webhook_id, "
                                "attempt, status, signature, error) VALUES ($1::uuid, $2::uuid, "
                                "$3, $4, $5, $6) RETURNING ") +
                        application_webhook_delivery_columns,
                    input.event_id,
                    input.webhook_id,
                    input.attempt,
                    input.status,
                    input.signature,
                    input.error);
                return application_webhook_delivery_of(rows[0]);
            });
    }

    Result<std::vector<ApplicationWebhookDelivery>>
    list_application_webhook_deliveries(const std::string& event_id) override {
        return with_tx<std::vector<ApplicationWebhookDelivery>>(
            [&](pqxx::work& tx) -> Result<std::vector<ApplicationWebhookDelivery>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + application_webhook_delivery_columns +
                        " FROM auth_app_webhook_deliveries WHERE event_id = $1::uuid ORDER BY "
                        "created_at, attempt",
                    event_id);
                std::vector<ApplicationWebhookDelivery> output;
                for (const auto& row : rows) {
                    output.push_back(application_webhook_delivery_of(row));
                }
                return output;
            });
    }

    Result<void>
    create_application_authorization_code(const ApplicationAuthorizationCode& value) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params(
                "INSERT INTO auth_app_authorization_codes "
                "(code_hash, application_id, user_id, redirect_uri, code_challenge, nonce) "
                "VALUES ($1, $2::uuid, $3::uuid, $4, $5, $6)",
                value.code_hash,
                value.application_id,
                value.user_id,
                value.redirect_uri,
                value.code_challenge,
                value.nonce);
            return {};
        });
    }

    Result<ApplicationAuthorizationCode>
    consume_application_authorization_code(const std::string& application_id,
                                           const std::string& code_hash) override {
        return with_tx<ApplicationAuthorizationCode>(
            [&](pqxx::work& tx) -> Result<ApplicationAuthorizationCode> {
                const pqxx::result rows = tx.exec_params(
                    "UPDATE auth_app_authorization_codes SET used_at = now() "
                    "WHERE application_id = $1::uuid AND code_hash = $2 AND used_at IS NULL "
                    "AND expires_at > now() RETURNING application_id::text, user_id::text, "
                    "redirect_uri, code_hash, code_challenge, nonce",
                    application_id,
                    code_hash);
                if (rows.empty()) {
                    return std::unexpected(StoreError{
                        "unauthorized", "authorization code is invalid, expired, or already used"});
                }
                return ApplicationAuthorizationCode{rows[0][0].as<std::string>(),
                                                    rows[0][1].as<std::string>(),
                                                    rows[0][2].as<std::string>(),
                                                    rows[0][3].as<std::string>(),
                                                    rows[0][4].as<std::string>(),
                                                    rows[0][5].as<std::string>()};
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

    Result<MeteringUsage> upsert_metering_usage(const MeteringUpdate& update) override {
        return with_tx<MeteringUsage>([&](pqxx::work& tx) -> Result<MeteringUsage> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO metering_usage "
                "(customer_id, hour, stored_bytes, transfer_in_bytes, transfer_out_bytes) "
                "VALUES ($1::uuid, $2::timestamptz, COALESCE($3::bigint, 0), $4, $5) "
                "ON CONFLICT (customer_id, hour) DO UPDATE SET "
                "stored_bytes = COALESCE($3::bigint, metering_usage.stored_bytes), "
                "transfer_in_bytes = metering_usage.transfer_in_bytes + "
                "EXCLUDED.transfer_in_bytes, "
                "transfer_out_bytes = metering_usage.transfer_out_bytes + "
                "EXCLUDED.transfer_out_bytes "
                "RETURNING customer_id::text, "
                "to_char(hour AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "stored_bytes, transfer_in_bytes, transfer_out_bytes, compute_seconds",
                update.customer_id,
                update.hour,
                update.stored_bytes,
                update.transfer_in_bytes,
                update.transfer_out_bytes);
            return MeteringUsage{.customer_id = rows[0][0].as<std::string>(),
                                 .hour = rows[0][1].as<std::string>(),
                                 .stored_bytes = rows[0][2].as<std::uint64_t>(),
                                 .transfer_in_bytes = rows[0][3].as<std::uint64_t>(),
                                 .transfer_out_bytes = rows[0][4].as<std::uint64_t>(),
                                 .compute_seconds = rows[0][5].as<std::uint64_t>()};
        });
    }

    Result<bool> record_metered_build(const MeteredBuild& build) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            const pqxx::result inserted = tx.exec_params(
                "WITH inserted AS ("
                "INSERT INTO metering_build_records "
                "(customer_id, result_id, hour, compute_seconds) "
                "VALUES ($1::uuid, $2, $3::timestamptz, $4) "
                "ON CONFLICT DO NOTHING RETURNING customer_id, hour, compute_seconds), "
                "usage AS ("
                "INSERT INTO metering_usage (customer_id, hour, compute_seconds) "
                "SELECT customer_id, hour, compute_seconds FROM inserted "
                "ON CONFLICT (customer_id, hour) DO UPDATE SET "
                "compute_seconds = metering_usage.compute_seconds + EXCLUDED.compute_seconds "
                "RETURNING 1) SELECT count(*) FROM inserted",
                build.customer_id,
                build.result_id,
                build.hour,
                build.compute_seconds);
            return inserted[0][0].as<int>() == 1;
        });
    }

    Result<std::vector<MeteringUsage>> list_metering_usage(const std::string& customer_id,
                                                           const std::string& from_hour,
                                                           const std::string& to_hour,
                                                           int limit) override {
        return with_tx<std::vector<MeteringUsage>>(
            [&](pqxx::work& tx) -> Result<std::vector<MeteringUsage>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT customer_id::text, "
                    "to_char(hour AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                    "stored_bytes, transfer_in_bytes, transfer_out_bytes, compute_seconds "
                    "FROM metering_usage WHERE customer_id = $1::uuid "
                    "AND ($2 = '' OR hour >= $2::timestamptz) "
                    "AND ($3 = '' OR hour <= $3::timestamptz) ORDER BY hour ASC LIMIT $4",
                    customer_id,
                    from_hour,
                    to_hour,
                    limit);
                std::vector<MeteringUsage> usage;
                for (const pqxx::row& row : rows) {
                    usage.push_back(MeteringUsage{.customer_id = row[0].as<std::string>(),
                                                  .hour = row[1].as<std::string>(),
                                                  .stored_bytes = row[2].as<std::uint64_t>(),
                                                  .transfer_in_bytes = row[3].as<std::uint64_t>(),
                                                  .transfer_out_bytes = row[4].as<std::uint64_t>(),
                                                  .compute_seconds = row[5].as<std::uint64_t>()});
                }
                return usage;
            });
    }

    Result<std::optional<std::uint64_t>> storage_quota(const std::string& customer_id) override {
        return with_tx<std::optional<std::uint64_t>>(
            [&](pqxx::work& tx) -> Result<std::optional<std::uint64_t>> {
                const pqxx::result customers =
                    tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id);
                if (customers.empty()) {
                    return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
                }
                const pqxx::result rows = tx.exec_params(
                    "SELECT quota_bytes FROM customer_storage_quotas WHERE customer_id = $1::uuid",
                    customer_id);
                if (rows.empty()) {
                    return std::optional<std::uint64_t>{};
                }
                return rows[0][0].as<std::uint64_t>();
            });
    }

    Result<std::uint64_t> set_storage_quota(const std::string& customer_id,
                                            std::uint64_t quota_bytes) override {
        return with_tx<std::uint64_t>([&](pqxx::work& tx) -> Result<std::uint64_t> {
            const pqxx::result customers =
                tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO customer_storage_quotas (customer_id, quota_bytes) VALUES ($1::uuid, "
                "$2) "
                "ON CONFLICT (customer_id) DO UPDATE SET quota_bytes = EXCLUDED.quota_bytes, "
                "updated_at = now() RETURNING quota_bytes",
                customer_id,
                quota_bytes);
            return rows[0][0].as<std::uint64_t>();
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
            std::string application_id;
            if (in.pool_id != "lake") {
                const pqxx::result applications = tx.exec_params(
                    "SELECT id::text FROM auth_applications WHERE pool_id = $1", in.pool_id);
                if (applications.empty()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "application pool does not exist"});
                }
                application_id = applications[0][0].as<std::string>();
            }
            const pqxx::result user_rows =
                tx.exec_params("INSERT INTO users (taap_id, email, display_name, pool_id) "
                               "VALUES ($1, $2, $3, $4) "
                               "RETURNING id::text",
                               identity->identifier,
                               in.email,
                               in.display_name,
                               in.pool_id);
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
            if (!application_id.empty()) {
                tx.exec_params("INSERT INTO auth_app_users (application_id, user_id) VALUES "
                               "($1::uuid, $2::uuid)",
                               application_id,
                               user_id);
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
            if (const auto active = require_application_user_active_tx(tx, in.pool_id, user_id);
                !active) {
                return std::unexpected(active.error());
            }
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

    Result<IssuedSession> sign_in_taap(const TaapSignIn& input) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result users =
                tx.exec_params("SELECT id::text FROM users WHERE pool_id = $1 AND taap_id = $2",
                               input.pool_id,
                               input.taap_id);
            if (users.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "TAAP identity is not registered"});
            }
            const std::string user_id = users[0][0].as<std::string>();
            try {
                tx.exec_params("INSERT INTO identity_taap_assertions "
                               "(assertion_id, user_id, pool_id) VALUES ($1, $2::uuid, $3)",
                               input.assertion_id,
                               user_id,
                               input.pool_id);
            } catch (const pqxx::unique_violation&) {
                return std::unexpected(
                    StoreError{"unauthorized", "TAAP assertion was already used"});
            }
            return issue_session(
                tx, user_id, input.pool_id, input.device_name, "taap", "", {"*"}, "", 60 * 60 * 24);
        });
    }
    Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                     const std::string& pool_id) override {
        MfaEnrollment enrollment;
        enrollment.secret = identity_crypto::new_totp_secret();
        for (int index = 0; index < 8; ++index) {
            const std::string code = identity_crypto::random_secret().substr(0, 12);
            if (code.size() != 12) {
                return std::unexpected(
                    StoreError{"store_error", "could not create MFA recovery codes"});
            }
            enrollment.recovery_codes.push_back(code);
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
                const std::string hash = identity_crypto::hash_secret(code);
                if (hash.empty()) {
                    return std::unexpected(
                        StoreError{"store_error", "could not protect MFA recovery codes"});
                }
                tx.exec_params(
                    "INSERT INTO credentials (user_id, pool_id, kind, subject, secret_hash) "
                    "VALUES ($1::uuid, $2, 'recovery', $3, $4)",
                    user_id,
                    pool_id,
                    identity_crypto::random_secret(),
                    hash);
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
            if (!seed) {
                return std::unexpected(seed.error());
            }
            bool verified = identity_crypto::verify_totp(*seed, code);
            if (!verified) {
                const pqxx::result recovery_rows =
                    tx.exec_params("SELECT secret_hash FROM credentials WHERE user_id = $1::uuid "
                                   "AND pool_id = $2 AND kind = 'recovery' AND revoked_at IS NULL",
                                   user_id,
                                   pool_id);
                verified = std::any_of(
                    recovery_rows.begin(), recovery_rows.end(), [&](const pqxx::row& row) {
                        return identity_crypto::verify_secret(row[0].as<std::string>(), code);
                    });
            }
            if (!verified) {
                return std::unexpected(
                    StoreError{"unauthorized", "a fresh MFA verification is required"});
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
            if (const auto active = require_application_user_active_tx(
                    tx, in.pool_id, password_rows[0][0].as<std::string>());
                !active) {
                return std::unexpected(active.error());
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
        const Result<User> user = get_user(in.user_id);
        if (!user || user->taap_id.empty()) {
            return std::unexpected(StoreError{"not_found", "user TAAP identity not found"});
        }
        const auto binding = audit::sign_passkey_binding(
            *this, user->taap_id, verified->credential_id, in.pool_id, in.identity_options);
        if (!binding) {
            return std::unexpected(StoreError{binding.error().code, binding.error().message});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result device = tx.exec_params(
                "INSERT INTO devices (user_id, name) VALUES ($1::uuid, $2) RETURNING id::text",
                in.user_id,
                in.name.empty() ? "passkey" : in.name);
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, "
                           "subject, public_data) "
                           "VALUES ($1::uuid, $2, 'passkey', $3, "
                           "jsonb_build_object('public_key', $4::text, 'name', "
                           "$5::text, "
                           "'sign_count', $6::integer, 'device_id', $7::text, "
                           "'binding_attestation', $8::jsonb))",
                           in.user_id,
                           in.pool_id,
                           verified->credential_id,
                           verified->public_key_pem,
                           in.name,
                           verified->sign_count,
                           device[0][0].as<std::string>(),
                           binding->dump());
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
            if (const auto active = require_application_user_active_tx(
                    tx, in.pool_id, rows[0][0].as<std::string>());
                !active) {
                return std::unexpected(active.error());
            }
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
            std::string application_id;
            if (in.pool_id != "lake") {
                const pqxx::result applications = tx.exec_params(
                    "SELECT id::text FROM auth_applications WHERE pool_id = $1", in.pool_id);
                if (applications.empty()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "application pool does not exist"});
                }
                application_id = applications[0][0].as<std::string>();
            }
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
                rows = tx.exec_params("INSERT INTO users (taap_id, email, display_name, pool_id) "
                                      "VALUES ($1, $2, $3, $4) RETURNING id::text",
                                      identity->identifier,
                                      nullable(in.email),
                                      in.display_name,
                                      in.pool_id);
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
                if (!application_id.empty()) {
                    tx.exec_params("INSERT INTO auth_app_users (application_id, user_id) VALUES "
                                   "($1::uuid, $2::uuid)",
                                   application_id,
                                   user_id);
                }
            } else {
                user_id = rows[0][0].as<std::string>();
            }
            if (const auto active = require_application_user_active_tx(tx, in.pool_id, user_id);
                !active) {
                return std::unexpected(active.error());
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
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + device_columns +
                    " FROM devices LEFT JOIN credentials AS passkeys ON "
                    "passkeys.user_id = devices.user_id AND passkeys.kind = 'passkey' AND "
                    "passkeys.public_data->>'device_id' = devices.id::text "
                    "WHERE devices.user_id = $1::uuid ORDER BY devices.created_at DESC",
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
            tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE user_id = $1::uuid "
                           "AND kind = 'passkey' AND public_data->>'device_id' = $2 "
                           "AND revoked_at IS NULL",
                           user_id,
                           device_id);
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
                        "devices.revoked_at IS NULL) AND (sessions.pool_id = 'lake' OR EXISTS "
                        "(SELECT 1 FROM auth_applications JOIN auth_app_users ON "
                        "auth_app_users.application_id = auth_applications.id WHERE "
                        "auth_applications.pool_id = sessions.pool_id AND "
                        "auth_app_users.user_id = sessions.user_id AND "
                        "auth_app_users.status = 'active'))");
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

    Result<SsoProvider> register_sso_provider(const NewSsoProvider& input) override {
        if (input.organization_id.empty() || input.name.empty() || input.issuer.empty() ||
            input.scim_token.empty() || (input.protocol != "saml" && input.protocol != "oidc")) {
            return std::unexpected(
                StoreError{"invalid_input", "provider configuration is incomplete"});
        }
        if ((input.protocol == "saml" &&
             (input.metadata.empty() || input.signing_certificate.empty())) ||
            (input.protocol == "oidc" &&
             (input.client_id.empty() || input.client_secret.empty()))) {
            return std::unexpected(
                StoreError{"invalid_input", "provider protocol configuration is incomplete"});
        }
        return with_tx<SsoProvider>([&](pqxx::work& tx) -> Result<SsoProvider> {
            std::string encrypted_secret;
            if (input.protocol == "oidc") {
                const auto encrypted =
                    encrypt_secret(tx,
                                   "sso.oidc." + input.organization_id + "." + input.name,
                                   input.client_secret);
                if (!encrypted) {
                    return std::unexpected(encrypted.error());
                }
                encrypted_secret = *encrypted;
            }
            const std::string token_hash = identity_crypto::hash_secret(input.scim_token);
            if (token_hash.empty()) {
                return std::unexpected(StoreError{"store_error", "could not hash SCIM token"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO sso_providers (organization_id, pool_id, name, protocol, "
                            "issuer, ") +
                    "metadata, signing_certificate, client_id, encrypted_client_secret, "
                    "scim_token_hash, "
                    "attribute_mapping, jit_provisioning) VALUES ($1::uuid, $2, $3, $4, $5, $6, "
                    "$7, $8, "
                    "decode(NULLIF($9, ''), 'hex'), $10, $11::jsonb, $12) ON CONFLICT "
                    "(organization_id, name) "
                    "DO UPDATE SET pool_id = EXCLUDED.pool_id, protocol = EXCLUDED.protocol, "
                    "issuer = "
                    "EXCLUDED.issuer, metadata = EXCLUDED.metadata, signing_certificate = "
                    "EXCLUDED.signing_certificate, client_id = EXCLUDED.client_id, "
                    "encrypted_client_secret = "
                    "EXCLUDED.encrypted_client_secret, scim_token_hash = EXCLUDED.scim_token_hash, "
                    "attribute_mapping = EXCLUDED.attribute_mapping, jit_provisioning = "
                    "EXCLUDED.jit_provisioning, enabled = true RETURNING " +
                    sso_provider_columns,
                input.organization_id,
                input.pool_id.empty() ? "lake" : input.pool_id,
                input.name,
                input.protocol,
                input.issuer,
                input.metadata,
                input.signing_certificate,
                input.client_id,
                encrypted_secret,
                token_hash,
                input.attribute_mapping.dump(),
                input.jit_provisioning);
            return sso_provider_of(rows[0]);
        });
    }

    Result<std::vector<SsoProvider>> list_sso_providers(const std::string& organization_id,
                                                        int limit) override {
        return with_tx<std::vector<SsoProvider>>(
            [&](pqxx::work& tx) -> Result<std::vector<SsoProvider>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + sso_provider_columns +
                                       " FROM sso_providers WHERE organization_id = $1::uuid ORDER "
                                       "BY created_at DESC LIMIT $2",
                                   organization_id,
                                   limit);
                std::vector<SsoProvider> providers;
                for (const auto& row : rows) {
                    providers.push_back(sso_provider_of(row));
                }
                return providers;
            });
    }

    Result<SsoProvider> get_sso_provider(const std::string& id) override {
        return with_tx<SsoProvider>([&](pqxx::work& tx) -> Result<SsoProvider> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + sso_provider_columns +
                                                         " FROM sso_providers WHERE id = $1::uuid",
                                                     id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
            }
            return sso_provider_of(rows[0]);
        });
    }

    Result<OAuthProvider> get_sso_oidc_provider(const std::string& id) override {
        return with_tx<OAuthProvider>([&](pqxx::work& tx) -> Result<OAuthProvider> {
            const pqxx::result rows = tx.exec_params(
                "SELECT organization_id::text, name, pool_id, issuer, client_id, "
                "encode(encrypted_client_secret, 'hex') FROM sso_providers WHERE id = $1::uuid "
                "AND protocol = 'oidc' AND enabled",
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "OIDC provider is unavailable"});
            }
            const Result<std::string> secret = decrypt_secret(
                tx,
                "sso.oidc." + rows[0][0].as<std::string>() + "." + rows[0][1].as<std::string>(),
                rows[0][5].as<std::string>());
            if (!secret) {
                return std::unexpected(secret.error());
            }
            return OAuthProvider{rows[0][2].as<std::string>(),
                                 "sso-" + id,
                                 rows[0][3].as<std::string>(),
                                 rows[0][4].as<std::string>(),
                                 *secret,
                                 {"openid", "email", "profile"}};
        });
    }

    Result<SsoProvider> authenticate_scim(const std::string& bearer_token) override {
        return with_tx<SsoProvider>([&](pqxx::work& tx) -> Result<SsoProvider> {
            const pqxx::result rows = tx.exec(std::string("SELECT ") + sso_provider_columns +
                                              ", scim_token_hash FROM sso_providers "
                                              "WHERE enabled ORDER BY created_at");
            for (const pqxx::row& row : rows) {
                if (identity_crypto::verify_secret(row[15].as<std::string>(), bearer_token)) {
                    return sso_provider_of(row);
                }
            }
            return std::unexpected(StoreError{"unauthorized", "SCIM bearer token is invalid"});
        });
    }

    Result<SsoProvider> test_sso_provider(const std::string& id) override {
        return with_tx<SsoProvider>([&](pqxx::work& tx) -> Result<SsoProvider> {
            const pqxx::result rows = tx.exec_params(
                std::string(
                    "UPDATE sso_providers SET tested_at = now() WHERE id = $1::uuid AND enabled "
                    "AND ((protocol = 'saml' AND length(metadata) > 0 AND "
                    "length(signing_certificate) > 0) "
                    "OR (protocol = 'oidc' AND length(client_id) > 0 AND encrypted_client_secret "
                    "IS NOT NULL)) "
                    "RETURNING ") +
                    sso_provider_columns,
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"invalid_input", "SSO provider is not testable"});
            }
            return sso_provider_of(rows[0]);
        });
    }

    Result<FederationIssuer> create_federation_issuer(const NewFederationIssuer& input) override {
        if (input.organization_id.empty() || input.issuer.empty() || input.claim.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "organization, issuer, and claim are required"});
        }
        return with_tx<FederationIssuer>([&](pqxx::work& tx) -> Result<FederationIssuer> {
            const pqxx::result rows = tx.exec_params(
                std::string(
                    "INSERT INTO federation_issuers (organization_id, issuer, claim) VALUES ") +
                    "($1::uuid, $2, $3) RETURNING " + federation_issuer_columns,
                input.organization_id,
                input.issuer,
                input.claim);
            return federation_issuer_of(rows[0]);
        });
    }

    Result<std::vector<FederationIssuer>>
    list_federation_issuers(const std::string& organization_id, int limit) override {
        return with_tx<std::vector<FederationIssuer>>(
            [&](pqxx::work& tx) -> Result<std::vector<FederationIssuer>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + federation_issuer_columns +
                                       " FROM federation_issuers WHERE organization_id = $1::uuid "
                                       "ORDER BY created_at DESC LIMIT $2",
                                   organization_id,
                                   limit);
                std::vector<FederationIssuer> issuers;
                for (const auto& row : rows) {
                    issuers.push_back(federation_issuer_of(row));
                }
                return issuers;
            });
    }

    Result<bool> allows_federation_claim(const std::string& organization_id,
                                         const std::string& issuer,
                                         const std::string& claim) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            return !tx.exec_params("SELECT 1 FROM federation_issuers "
                                   "WHERE organization_id = $1::uuid AND issuer = $2 "
                                   "AND claim = $3",
                                   organization_id,
                                   issuer,
                                   claim)
                        .empty();
        });
    }

    Result<IssuedSession> sign_in_sso(const SsoIdentity& input) override {
        if (input.provider_id.empty() || input.issuer.empty() || input.subject.empty() ||
            !input.claims.is_object()) {
            return std::unexpected(StoreError{"invalid_input", "SSO assertion is incomplete"});
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result providers =
                tx.exec_params(std::string("SELECT ") + sso_provider_columns +
                                   " FROM sso_providers WHERE id = $1::uuid AND enabled",
                               input.provider_id);
            if (providers.empty()) {
                return std::unexpected(StoreError{"unauthorized", "SSO provider is unavailable"});
            }
            const SsoProvider provider = sso_provider_of(providers[0]);
            if (provider.issuer != input.issuer) {
                return std::unexpected(StoreError{"unauthorized", "SSO issuer is not accepted"});
            }
            for (auto claim = input.claims.begin(); claim != input.claims.end(); ++claim) {
                const pqxx::result permitted =
                    tx.exec_params("SELECT 1 FROM federation_issuers WHERE organization_id = "
                                   "$1::uuid AND issuer = $2 "
                                   "AND claim = $3",
                                   provider.organization_id,
                                   input.issuer,
                                   claim.key());
                if (permitted.empty()) {
                    return std::unexpected(StoreError{
                        "unauthorized", "issuer is not allowed to assert " + claim.key()});
                }
            }
            const pqxx::result mapped =
                tx.exec_params("SELECT scim_users.user_id::text, scim_users.active FROM scim_users "
                               "WHERE provider_id = "
                               "$1::uuid AND external_id = $2 FOR UPDATE",
                               input.provider_id,
                               input.subject);
            std::string user_id;
            if (!mapped.empty()) {
                if (!mapped[0][1].as<bool>()) {
                    return std::unexpected(
                        StoreError{"unauthorized", "provisioned user is inactive"});
                }
                user_id = mapped[0][0].as<std::string>();
            } else {
                if (!provider.jit_provisioning || input.taap_id.empty()) {
                    return std::unexpected(
                        StoreError{"unauthorized", "SSO user is not provisioned"});
                }
                const pqxx::result created =
                    tx.exec_params("INSERT INTO users (taap_id, email, display_name) VALUES ($1, "
                                   "$2, $3) RETURNING id::text",
                                   input.taap_id,
                                   nullable(input.email),
                                   input.display_name);
                user_id = created[0][0].as<std::string>();
                tx.exec_params(
                    "INSERT INTO scim_users (provider_id, external_id, user_id, attributes) "
                    "VALUES ($1::uuid, $2, $3::uuid, $4::jsonb)",
                    input.provider_id,
                    input.subject,
                    user_id,
                    input.claims.dump());
            }
            const pqxx::result user = tx.exec_params(
                "SELECT taap_id FROM users WHERE id = $1::uuid AND taap_id IS NOT NULL", user_id);
            if (user.empty()) {
                return std::unexpected(StoreError{"unauthorized", "SSO user has no TAAP identity"});
            }
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, subject) VALUES "
                           "($1::uuid, $2, $3, $4) ON CONFLICT (pool_id, kind, subject) DO NOTHING",
                           user_id,
                           provider.pool_id,
                           provider.protocol,
                           input.provider_id + ":" + input.subject);
            return issue_session(tx,
                                 user_id,
                                 provider.pool_id,
                                 input.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }

    Result<std::vector<ScimUser>> list_scim_users(const std::string& provider_id,
                                                  int limit) override {
        return with_tx<std::vector<ScimUser>>([&](pqxx::work& tx) -> Result<std::vector<ScimUser>> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + scim_user_columns +
                    " FROM scim_users WHERE provider_id = $1::uuid ORDER BY created_at LIMIT $2",
                provider_id,
                limit);
            std::vector<ScimUser> users;
            for (const pqxx::row& row : rows) {
                users.push_back(scim_user_of(row));
            }
            return users;
        });
    }

    Result<ScimUser> get_scim_user(const std::string& provider_id,
                                   const std::string& resource_id) override {
        return with_tx<ScimUser>([&](pqxx::work& tx) -> Result<ScimUser> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + scim_user_columns +
                                   " FROM scim_users WHERE provider_id = $1::uuid "
                                   "AND (user_id::text = $2 OR external_id = $2)",
                               provider_id,
                               resource_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
            }
            return scim_user_of(rows[0]);
        });
    }

    Result<ScimUser> upsert_scim_user(const NewScimUser& input) override {
        if (input.provider_id.empty() || input.external_id.empty() || input.taap_id.empty()) {
            return std::unexpected(StoreError{
                "invalid_input", "provider, external id, and TAAP identity are required"});
        }
        return with_tx<ScimUser>([&](pqxx::work& tx) -> Result<ScimUser> {
            const pqxx::result provider = tx.exec_params(
                "SELECT 1 FROM sso_providers WHERE id = $1::uuid", input.provider_id);
            if (provider.empty()) {
                return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
            }
            const pqxx::result existing =
                tx.exec_params("SELECT user_id::text FROM scim_users WHERE provider_id = $1::uuid "
                               "AND external_id = $2 "
                               "FOR UPDATE",
                               input.provider_id,
                               input.external_id);
            std::string user_id;
            std::string operation = "create";
            if (existing.empty()) {
                const pqxx::result created =
                    tx.exec_params("INSERT INTO users (taap_id, email, display_name) VALUES ($1, "
                                   "$2, $3) RETURNING id::text",
                                   input.taap_id,
                                   nullable(input.email),
                                   input.display_name);
                user_id = created[0][0].as<std::string>();
                tx.exec_params("INSERT INTO scim_users (provider_id, external_id, user_id, active, "
                               "attributes) "
                               "VALUES ($1::uuid, $2, $3::uuid, $4, $5::jsonb)",
                               input.provider_id,
                               input.external_id,
                               user_id,
                               input.active,
                               input.attributes.dump());
            } else {
                user_id = existing[0][0].as<std::string>();
                operation = input.active ? "update" : "deactivate";
                tx.exec_params("UPDATE users SET email = $2, display_name = $3 WHERE id = $1::uuid",
                               user_id,
                               nullable(input.email),
                               input.display_name);
                tx.exec_params("UPDATE scim_users SET active = $3, attributes = $4::jsonb WHERE "
                               "provider_id = $1::uuid AND external_id = $2",
                               input.provider_id,
                               input.external_id,
                               input.active,
                               input.attributes.dump());
            }
            if (!input.active) {
                remove_scim_user_access(tx, input.provider_id, input.external_id, user_id);
                operation = "deactivate";
            }
            tx.exec_params(
                "INSERT INTO sso_provisioning_logs (provider_id, resource, operation, external_id, "
                "outcome) VALUES ($1::uuid, 'user', $2, $3, 'success')",
                input.provider_id,
                operation,
                input.external_id);
            const pqxx::result row =
                tx.exec_params(std::string("SELECT ") + scim_user_columns +
                                   " FROM scim_users WHERE provider_id = $1::uuid "
                                   "AND external_id = $2",
                               input.provider_id,
                               input.external_id);
            return scim_user_of(row[0]);
        });
    }

    Result<ScimUser> delete_scim_user(const std::string& provider_id,
                                      const std::string& resource_id) override {
        return with_tx<ScimUser>([&](pqxx::work& tx) -> Result<ScimUser> {
            const pqxx::result existing =
                tx.exec_params(std::string("SELECT ") + scim_user_columns +
                                   " FROM scim_users WHERE provider_id = $1::uuid "
                                   "AND (user_id::text = $2 OR external_id = $2) FOR UPDATE",
                               provider_id,
                               resource_id);
            if (existing.empty()) {
                return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
            }
            const ScimUser user = scim_user_of(existing[0]);
            tx.exec_params("UPDATE scim_users SET active = false WHERE provider_id = $1::uuid "
                           "AND external_id = $2",
                           provider_id,
                           user.external_id);
            remove_scim_user_access(tx, provider_id, user.external_id, user.user_id);
            tx.exec_params(
                "INSERT INTO sso_provisioning_logs (provider_id, resource, operation, "
                "external_id, outcome) VALUES ($1::uuid, 'user', 'deactivate', $2, 'success')",
                provider_id,
                user.external_id);
            const pqxx::result updated = tx.exec_params(
                std::string("SELECT ") + scim_user_columns +
                    " FROM scim_users WHERE provider_id = $1::uuid AND external_id = $2",
                provider_id,
                user.external_id);
            return scim_user_of(updated[0]);
        });
    }

    Result<std::vector<ScimGroup>> list_scim_groups(const std::string& provider_id,
                                                    int limit) override {
        return with_tx<std::vector<ScimGroup>>(
            [&](pqxx::work& tx) -> Result<std::vector<ScimGroup>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + scim_group_columns +
                                       " FROM scim_groups WHERE provider_id = $1::uuid "
                                       "ORDER BY created_at LIMIT $2",
                                   provider_id,
                                   limit);
                std::vector<ScimGroup> groups;
                for (const pqxx::row& row : rows) {
                    groups.push_back(scim_group_of(row));
                }
                return groups;
            });
    }

    Result<ScimGroup> get_scim_group(const std::string& provider_id,
                                     const std::string& resource_id) override {
        return with_tx<ScimGroup>([&](pqxx::work& tx) -> Result<ScimGroup> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + scim_group_columns +
                                   " FROM scim_groups WHERE provider_id = $1::uuid "
                                   "AND (team_id::text = $2 OR external_id = $2)",
                               provider_id,
                               resource_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
            }
            return scim_group_of(rows[0]);
        });
    }

    Result<ScimGroup> upsert_scim_group(const NewScimGroup& input) override {
        if (input.provider_id.empty() || input.external_id.empty() || input.team_name.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "provider, external id, and team name are required"});
        }
        return with_tx<ScimGroup>([&](pqxx::work& tx) -> Result<ScimGroup> {
            const pqxx::result providers = tx.exec_params(
                "SELECT organization_id::text, issuer FROM sso_providers WHERE id = $1::uuid",
                input.provider_id);
            if (providers.empty()) {
                return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
            }
            const std::string organization_id = providers[0][0].as<std::string>();
            const std::string issuer = providers[0][1].as<std::string>();
            pqxx::result groups = tx.exec_params("SELECT team_id::text FROM scim_groups WHERE "
                                                 "provider_id = $1::uuid AND external_id = $2 "
                                                 "FOR UPDATE",
                                                 input.provider_id,
                                                 input.external_id);
            const bool creating = groups.empty();
            std::string team_id;
            if (groups.empty()) {
                const pqxx::result team =
                    tx.exec_params("INSERT INTO teams (organization_id, name, display_name) VALUES "
                                   "($1::uuid, $2, $3) "
                                   "RETURNING id::text",
                                   organization_id,
                                   input.team_name,
                                   input.display_name);
                team_id = team[0][0].as<std::string>();
                const pqxx::result parent =
                    tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                                   "'organization' AND external_id = $1",
                                   organization_id);
                if (parent.empty()) {
                    return std::unexpected(
                        StoreError{"store_error", "organization policy object is missing"});
                }
                tx.exec_params(
                    "INSERT INTO objects (object_type, external_id, parent_id) VALUES ('team', $1, "
                    "$2::uuid)",
                    team_id,
                    parent[0][0].as<std::string>());
                tx.exec_params(
                    "INSERT INTO scim_groups (provider_id, external_id, team_id, display_name, "
                    "attributes) VALUES ($1::uuid, $2, $3::uuid, $4, $5::jsonb)",
                    input.provider_id,
                    input.external_id,
                    team_id,
                    input.display_name,
                    input.attributes.dump());
            } else {
                team_id = groups[0][0].as<std::string>();
                tx.exec_params(
                    "UPDATE scim_groups SET display_name = $3, attributes = $4::jsonb WHERE "
                    "provider_id = $1::uuid AND external_id = $2",
                    input.provider_id,
                    input.external_id,
                    input.display_name,
                    input.attributes.dump());
            }
            const pqxx::result team_object = tx.exec_params(
                "SELECT id::text FROM objects WHERE object_type = 'team' AND external_id = $1",
                team_id);
            if (team_object.empty()) {
                return std::unexpected(StoreError{"store_error", "team policy object is missing"});
            }
            const pqxx::result existing_members = tx.exec_params(
                "SELECT scim_group_members.user_external_id, users.taap_id, "
                "scim_group_members.membership_id::text FROM "
                "scim_group_members JOIN scim_users ON scim_users.provider_id = "
                "scim_group_members.provider_id AND scim_users.external_id = "
                "scim_group_members.user_external_id JOIN users ON users.id = scim_users.user_id "
                "WHERE scim_group_members.provider_id = $1::uuid AND "
                "scim_group_members.group_external_id = $2",
                input.provider_id,
                input.external_id);
            const std::string digest = "scim:" + input.provider_id + ":" + input.external_id;
            for (const pqxx::row& member : existing_members) {
                const std::string external_id = member[0].as<std::string>();
                if (std::ranges::find(input.member_external_ids, external_id) !=
                    input.member_external_ids.end()) {
                    continue;
                }
                tx.exec_params("DELETE FROM grants WHERE object_id = $1::uuid AND "
                               "principal_taap_id = $2 AND attestation_digest = $3",
                               team_object[0][0].as<std::string>(),
                               member[1].as<std::string>(),
                               digest);
                tx.exec_params("DELETE FROM memberships WHERE id = $1::uuid",
                               member[2].as<std::string>());
            }
            for (const std::string& external_id : input.member_external_ids) {
                const pqxx::result user = tx.exec_params(
                    "SELECT scim_users.user_id::text, users.taap_id FROM scim_users JOIN users ON "
                    "users.id = scim_users.user_id WHERE scim_users.provider_id = $1::uuid AND "
                    "scim_users.external_id = $2 AND scim_users.active",
                    input.provider_id,
                    external_id);
                if (user.empty() || user[0][1].is_null()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "SCIM group member is not active"});
                }
                pqxx::result membership =
                    tx.exec_params(std::string("INSERT INTO memberships (organization_id, team_id, "
                                               "principal_type, principal_id, ") +
                                       "principal_taap_id, role, attestation_digest, "
                                       "granted_by_taap_id) VALUES ($1::uuid, "
                                       "$2::uuid, 'user', $3, $4, 'member', $5, $6) ON CONFLICT DO "
                                       "NOTHING RETURNING " +
                                       membership_columns,
                                   organization_id,
                                   team_id,
                                   user[0][0].as<std::string>(),
                                   user[0][1].as<std::string>(),
                                   digest,
                                   issuer);
                if (membership.empty()) {
                    membership = tx.exec_params(std::string("SELECT ") + membership_columns +
                                                    " FROM memberships WHERE "
                                                    "organization_id = $1::uuid AND team_id = "
                                                    "$2::uuid AND principal_type = 'user' "
                                                    "AND principal_id = $3",
                                                organization_id,
                                                team_id,
                                                user[0][0].as<std::string>());
                }
                tx.exec_params(
                    "INSERT INTO grants (principal_taap_id, object_id, role, attestation_digest, "
                    "granted_by_taap_id) VALUES ($1, $2::uuid, 'read', $3, $4) ON CONFLICT DO "
                    "NOTHING",
                    user[0][1].as<std::string>(),
                    team_object[0][0].as<std::string>(),
                    digest,
                    issuer);
                tx.exec_params(
                    "INSERT INTO scim_group_members (provider_id, group_external_id, "
                    "user_external_id, "
                    "membership_id) VALUES ($1::uuid, $2, $3, $4::uuid) ON CONFLICT DO NOTHING",
                    input.provider_id,
                    input.external_id,
                    external_id,
                    membership[0][0].as<std::string>());
            }
            tx.exec_params(
                "INSERT INTO sso_provisioning_logs (provider_id, resource, operation, external_id, "
                "outcome) VALUES ($1::uuid, 'group', $3, $2, 'success')",
                input.provider_id,
                input.external_id,
                creating ? "create" : "update");
            const pqxx::result row =
                tx.exec_params(std::string("SELECT ") + scim_group_columns +
                                   " FROM scim_groups WHERE provider_id = $1::uuid "
                                   "AND external_id = $2",
                               input.provider_id,
                               input.external_id);
            return scim_group_of(row[0]);
        });
    }

    Result<void> delete_scim_group(const std::string& provider_id,
                                   const std::string& resource_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result existing = tx.exec_params(
                "SELECT external_id, team_id::text FROM scim_groups WHERE provider_id = $1::uuid "
                "AND (team_id::text = $2 OR external_id = $2) FOR UPDATE",
                provider_id,
                resource_id);
            if (existing.empty()) {
                return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
            }
            const std::string external_id = existing[0][0].as<std::string>();
            const std::string team_id = existing[0][1].as<std::string>();
            tx.exec_params("DELETE FROM objects WHERE object_type = 'team' AND external_id = $1",
                           team_id);
            tx.exec_params("DELETE FROM teams WHERE id = $1::uuid", team_id);
            tx.exec_params(
                "INSERT INTO sso_provisioning_logs (provider_id, resource, operation, "
                "external_id, outcome) VALUES ($1::uuid, 'group', 'deactivate', $2, 'success')",
                provider_id,
                external_id);
            return {};
        });
    }

    Result<std::vector<ProvisioningLog>> list_sso_provisioning_logs(const std::string& provider_id,
                                                                    int limit) override {
        return with_tx<std::vector<ProvisioningLog>>(
            [&](pqxx::work& tx) -> Result<std::vector<ProvisioningLog>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + provisioning_log_columns +
                                       " FROM sso_provisioning_logs WHERE provider_id = $1::uuid "
                                       "ORDER BY created_at DESC LIMIT $2",
                                   provider_id,
                                   limit);
                std::vector<ProvisioningLog> logs;
                for (const auto& row : rows) {
                    logs.push_back(provisioning_log_of(row));
                }
                return logs;
            });
    }

    Result<PkiAuthorityRecord> create_pki_authority(const NewPkiAuthorityRecord& input,
                                                    const std::string& instance_kek) override {
        if (input.customer_id.empty() || input.root_certificate_pem.empty() ||
            input.intermediate_chain_pem.empty() || input.root_private_key_pem.empty() ||
            input.intermediate_private_key_pem.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "customer and authority material are required"});
        }
        return with_tx<PkiAuthorityRecord>([&](pqxx::work& tx) -> Result<PkiAuthorityRecord> {
            auto customer_key = customer_kek_tx(tx, input.customer_id, instance_kek, true);
            if (!customer_key) {
                return std::unexpected(customer_key.error());
            }
            const std::string root_associated = "lake pki authority/" + input.customer_id + "/root";
            const std::string intermediate_associated =
                "lake pki authority/" + input.customer_id + "/intermediate";
            auto root =
                secrets_crypto::seal(*customer_key, input.root_private_key_pem, root_associated);
            auto intermediate = secrets_crypto::seal(
                *customer_key, input.intermediate_private_key_pem, intermediate_associated);
            secrets_crypto::cleanse(*customer_key);
            if (!root || !intermediate) {
                return std::unexpected(!root ? root.error() : intermediate.error());
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO pki_authorities "
                "(customer_id, root_key_ciphertext, intermediate_ciphertext, "
                "root_certificate_pem, intermediate_chain_pem) "
                "VALUES ($1::uuid, $2, $3, $4, $5) "
                "ON CONFLICT (customer_id) DO NOTHING "
                "RETURNING id::text, customer_id::text, root_certificate_pem, "
                "intermediate_chain_pem, root_key_ciphertext, intermediate_ciphertext, "
                "created_at::text, updated_at::text",
                input.customer_id,
                *root,
                *intermediate,
                input.root_certificate_pem,
                input.intermediate_chain_pem);
            if (rows.empty()) {
                return std::unexpected(StoreError{"conflict", "customer authority already exists"});
            }
            return PkiAuthorityRecord{rows[0][0].as<std::string>(),
                                      rows[0][1].as<std::string>(),
                                      rows[0][2].as<std::string>(),
                                      rows[0][3].as<std::string>(),
                                      rows[0][4].as<std::string>(),
                                      rows[0][5].as<std::string>(),
                                      rows[0][6].as<std::string>(),
                                      rows[0][7].as<std::string>()};
        });
    }

    Result<PkiAuthorityMaterial> get_pki_authority(const std::string& customer_id,
                                                   const std::string& instance_kek) override {
        return with_tx<PkiAuthorityMaterial>([&](pqxx::work& tx) -> Result<PkiAuthorityMaterial> {
            const pqxx::result rows = tx.exec_params(
                "SELECT root_certificate_pem, intermediate_chain_pem, root_key_ciphertext, "
                "intermediate_ciphertext FROM pki_authorities WHERE customer_id = $1::uuid",
                customer_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "customer authority does not exist"});
            }
            auto customer_key = customer_kek_tx(tx, customer_id, instance_kek, false);
            if (!customer_key) {
                return std::unexpected(customer_key.error());
            }
            const std::string root_associated = "lake pki authority/" + customer_id + "/root";
            const std::string intermediate_associated =
                "lake pki authority/" + customer_id + "/intermediate";
            auto root =
                secrets_crypto::open(*customer_key, rows[0][2].as<std::string>(), root_associated);
            auto intermediate = secrets_crypto::open(
                *customer_key, rows[0][3].as<std::string>(), intermediate_associated);
            secrets_crypto::cleanse(*customer_key);
            if (!root || !intermediate) {
                return std::unexpected(!root ? root.error() : intermediate.error());
            }
            return PkiAuthorityMaterial{customer_id,
                                        rows[0][0].as<std::string>(),
                                        rows[0][1].as<std::string>(),
                                        std::move(*root),
                                        std::move(*intermediate)};
        });
    }

    Result<PkiCallerRecord> create_pki_caller(const NewPkiCallerRecord& input) override {
        if (input.customer_id.empty() || input.workload_taap_id.empty() ||
            input.attestation_type.empty() || input.subject.empty() ||
            !input.attributes.is_object() || input.expires_at.empty()) {
            return std::unexpected(StoreError{"invalid_input", "caller record is invalid"});
        }
        return with_tx<PkiCallerRecord>([&](pqxx::work& tx) -> Result<PkiCallerRecord> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO pki_callers "
                "(customer_id, workload_taap_id, attestation_type, issuer, subject, attributes, "
                "expires_at) VALUES ($1::uuid, $2, $3, $4, $5, $6::jsonb, $7::timestamptz) "
                "RETURNING id::text, customer_id::text, workload_taap_id, attestation_type, "
                "issuer, subject, attributes::text, expires_at::text, issued_at::text",
                input.customer_id,
                input.workload_taap_id,
                input.attestation_type,
                input.issuer,
                input.subject,
                input.attributes.dump(),
                input.expires_at);
            return PkiCallerRecord{rows[0][0].as<std::string>(),
                                   rows[0][1].as<std::string>(),
                                   rows[0][2].as<std::string>(),
                                   rows[0][3].as<std::string>(),
                                   rows[0][4].as<std::string>(),
                                   rows[0][5].as<std::string>(),
                                   nlohmann::json::parse(rows[0][6].as<std::string>()),
                                   rows[0][7].as<std::string>(),
                                   rows[0][8].as<std::string>()};
        });
    }

    Result<std::vector<PkiCallerRecord>> list_pki_callers(const std::string& customer_id,
                                                          int limit) override {
        if (limit < 1 || limit > 100) {
            return std::unexpected(StoreError{"invalid_input", "limit must be between 1 and 100"});
        }
        return with_tx<std::vector<PkiCallerRecord>>(
            [&](pqxx::work& tx) -> Result<std::vector<PkiCallerRecord>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT id::text, customer_id::text, workload_taap_id, attestation_type, "
                    "issuer, subject, attributes::text, expires_at::text, issued_at::text "
                    "FROM pki_callers WHERE customer_id = $1::uuid "
                    "ORDER BY issued_at DESC, id DESC LIMIT $2",
                    customer_id,
                    limit);
                std::vector<PkiCallerRecord> output;
                output.reserve(rows.size());
                for (const pqxx::row& row : rows) {
                    output.push_back(
                        PkiCallerRecord{row[0].as<std::string>(),
                                        row[1].as<std::string>(),
                                        row[2].as<std::string>(),
                                        row[3].as<std::string>(),
                                        row[4].as<std::string>(),
                                        row[5].as<std::string>(),
                                        nlohmann::json::parse(row[6].as<std::string>()),
                                        row[7].as<std::string>(),
                                        row[8].as<std::string>()});
                }
                return output;
            });
    }

    Result<PkiCertificateRecord>
    create_pki_certificate(const NewPkiCertificateRecord& input) override {
        return with_tx<PkiCertificateRecord>([&](pqxx::work& tx) -> Result<PkiCertificateRecord> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO pki_certificates "
                "(authority_id, customer_id, profile, subject, serial, public_value, expires_at, "
                "renewed_from_id) SELECT id, customer_id, $2, $3, $4, $5, $6::timestamptz, "
                "NULLIF($7, '')::uuid FROM pki_authorities WHERE customer_id = $1::uuid "
                "RETURNING id::text, authority_id::text, customer_id::text, profile, subject, "
                "serial, public_value, "
                "to_char(expires_at AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), issued_at::text, "
                "coalesce(renewed_from_id::text,''), coalesce(revoked_at::text,'')",
                input.customer_id,
                input.profile,
                input.subject,
                input.serial,
                input.public_value,
                input.expires_at,
                input.renewed_from_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "customer authority does not exist"});
            }
            return PkiCertificateRecord{rows[0][0].as<std::string>(),
                                        rows[0][1].as<std::string>(),
                                        rows[0][2].as<std::string>(),
                                        rows[0][3].as<std::string>(),
                                        rows[0][4].as<std::string>(),
                                        rows[0][5].as<std::string>(),
                                        rows[0][6].as<std::string>(),
                                        rows[0][7].as<std::string>(),
                                        rows[0][8].as<std::string>(),
                                        rows[0][9].as<std::string>(),
                                        rows[0][10].as<std::string>()};
        });
    }

    Result<PkiCertificateRecord> get_pki_certificate(const std::string& id) override {
        return with_tx<PkiCertificateRecord>([&](pqxx::work& tx) -> Result<PkiCertificateRecord> {
            const pqxx::result rows = tx.exec_params(
                "SELECT id::text, authority_id::text, customer_id::text, profile, subject, serial, "
                "public_value, to_char(expires_at AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), issued_at::text, "
                "coalesce(renewed_from_id::text,''), coalesce(revoked_at::text,'') "
                "FROM pki_certificates WHERE id = $1::uuid",
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "certificate does not exist"});
            }
            return PkiCertificateRecord{rows[0][0].as<std::string>(),
                                        rows[0][1].as<std::string>(),
                                        rows[0][2].as<std::string>(),
                                        rows[0][3].as<std::string>(),
                                        rows[0][4].as<std::string>(),
                                        rows[0][5].as<std::string>(),
                                        rows[0][6].as<std::string>(),
                                        rows[0][7].as<std::string>(),
                                        rows[0][8].as<std::string>(),
                                        rows[0][9].as<std::string>(),
                                        rows[0][10].as<std::string>()};
        });
    }

    Result<PkiCertificateRecord> revoke_pki_certificate(const std::string& id) override {
        return with_tx<PkiCertificateRecord>([&](pqxx::work& tx) -> Result<PkiCertificateRecord> {
            tx.exec_params("UPDATE pki_certificates SET revoked_at = coalesce(revoked_at, now()) "
                           "WHERE id = $1::uuid",
                           id);
            return get_pki_certificate_tx(tx, id);
        });
    }

    Result<AgentSession> create_agent_session(const NewAgentSession& input) override {
        if (input.id.empty() || input.customer_id.empty() || input.durable_agent_taap_id.empty() ||
            input.dispatcher_taap_id.empty() || input.session_taap_id.empty() ||
            input.public_key.empty() || input.scopes.empty() || input.attestation.empty() ||
            input.transparency_root.empty() || input.expires_unix <= 0 || input.issued_at.empty() ||
            input.expires_at.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent session record is invalid"});
        }
        return with_tx<AgentSession>([&](pqxx::work& tx) -> Result<AgentSession> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO agent_sessions "
                "(id, customer_id, durable_agent_taap_id, dispatcher_taap_id, session_taap_id, "
                "public_key, scopes, attestation, transparency_index, transparency_root, "
                "issued_at, expires_at) VALUES "
                "($1::uuid, $2::uuid, $3, $4, $5, $6, "
                "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, $9, $10, "
                "$11::timestamptz, $12::timestamptz) RETURNING "
                "id::text, customer_id::text, durable_agent_taap_id, dispatcher_taap_id, "
                "session_taap_id, public_key, array_to_json(scopes)::text, attestation, "
                "transparency_index, transparency_root, extract(epoch from expires_at)::bigint, "
                "to_char(issued_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",
                input.id,
                input.customer_id,
                input.durable_agent_taap_id,
                input.dispatcher_taap_id,
                input.session_taap_id,
                input.public_key,
                nlohmann::json(input.scopes).dump(),
                input.attestation,
                input.transparency_index,
                input.transparency_root,
                input.issued_at,
                input.expires_at);
            return agent_session_of(rows[0]);
        });
    }

    Result<AgentSession> get_agent_session(const std::string& customer_id,
                                           const std::string& id) override {
        if (customer_id.empty() || id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent session id is required"});
        }
        return with_tx<AgentSession>([&](pqxx::work& tx) -> Result<AgentSession> {
            const pqxx::result rows = tx.exec_params(
                "SELECT id::text, customer_id::text, durable_agent_taap_id, "
                "dispatcher_taap_id, session_taap_id, public_key, "
                "array_to_json(scopes)::text, attestation, transparency_index, "
                "transparency_root, extract(epoch from expires_at)::bigint, "
                "to_char(issued_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
                "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                "FROM agent_sessions "
                "WHERE customer_id = $1::uuid AND id = $2::uuid",
                customer_id,
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "agent session does not exist"});
            }
            return agent_session_of(rows[0]);
        });
    }

    Result<PkiRecoveryCeremonyRecord>
    create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& input) override {
        if (input.id.empty() || input.customer_id.empty() || input.threshold < 2 ||
            input.share_count < input.threshold || input.share_count > 255 ||
            input.expires_at.empty()) {
            return std::unexpected(StoreError{"invalid_input", "recovery ceremony is invalid"});
        }
        return with_tx<PkiRecoveryCeremonyRecord>(
            [&](pqxx::work& tx) -> Result<PkiRecoveryCeremonyRecord> {
                const pqxx::result rows = tx.exec_params(
                    "INSERT INTO pki_recovery_ceremonies "
                    "(id, customer_id, authority_id, threshold, share_count, expires_at) "
                    "SELECT $1::uuid, $2::uuid, id, $3, $4, $5::timestamptz "
                    "FROM pki_authorities WHERE customer_id = $2::uuid "
                    "RETURNING id::text, customer_id::text, authority_id::text, threshold, "
                    "share_count, recovered_at::text, expires_at::text, created_at::text",
                    input.id,
                    input.customer_id,
                    input.threshold,
                    input.share_count,
                    input.expires_at);
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "customer authority does not exist"});
                }
                return PkiRecoveryCeremonyRecord{rows[0][0].as<std::string>(),
                                                 rows[0][1].as<std::string>(),
                                                 rows[0][2].as<std::string>(),
                                                 rows[0][3].as<int>(),
                                                 rows[0][4].as<int>(),
                                                 std::nullopt,
                                                 rows[0][6].as<std::string>(),
                                                 rows[0][7].as<std::string>()};
            });
    }

    Result<PkiRecoveryCeremonyRecord>
    get_pki_recovery_ceremony(const std::string& customer_id,
                              const std::string& ceremony_id) override {
        return with_tx<PkiRecoveryCeremonyRecord>(
            [&](pqxx::work& tx) -> Result<PkiRecoveryCeremonyRecord> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT id::text, customer_id::text, authority_id::text, threshold, "
                    "share_count, recovered_at::text, expires_at::text, created_at::text "
                    "FROM pki_recovery_ceremonies WHERE customer_id = $1::uuid AND id = $2::uuid",
                    customer_id,
                    ceremony_id);
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "recovery ceremony does not exist"});
                }
                const std::optional<std::string> recovered =
                    rows[0][5].is_null() ? std::nullopt
                                         : std::optional<std::string>(rows[0][5].as<std::string>());
                return PkiRecoveryCeremonyRecord{rows[0][0].as<std::string>(),
                                                 rows[0][1].as<std::string>(),
                                                 rows[0][2].as<std::string>(),
                                                 rows[0][3].as<int>(),
                                                 rows[0][4].as<int>(),
                                                 recovered,
                                                 rows[0][6].as<std::string>(),
                                                 rows[0][7].as<std::string>()};
            });
    }

    Result<PkiRecoveryCeremonyRecord>
    complete_pki_recovery_ceremony(const std::string& customer_id,
                                   const std::string& ceremony_id,
                                   const std::vector<PkiRecoveryShareEvidence>& evidence) override {
        return with_tx<PkiRecoveryCeremonyRecord>([&](pqxx::work& tx)
                                                      -> Result<PkiRecoveryCeremonyRecord> {
            const pqxx::result active = tx.exec_params(
                "SELECT threshold FROM pki_recovery_ceremonies "
                "WHERE customer_id = $1::uuid AND id = $2::uuid AND recovered_at IS NULL "
                "AND expires_at > now() FOR UPDATE",
                customer_id,
                ceremony_id);
            if (active.empty()) {
                return std::unexpected(StoreError{"conflict", "recovery ceremony is not active"});
            }
            if (static_cast<int>(evidence.size()) < active[0][0].as<int>()) {
                return std::unexpected(
                    StoreError{"invalid_input", "recovery quorum is not present"});
            }
            std::set<int> indices;
            for (const PkiRecoveryShareEvidence& share : evidence) {
                if (share.index < 1 || share.digest.empty() ||
                    !indices.insert(share.index).second) {
                    return std::unexpected(
                        StoreError{"invalid_input", "recovery evidence is invalid"});
                }
                tx.exec_params("INSERT INTO pki_recovery_shares "
                               "(ceremony_id, share_index, share_digest) "
                               "VALUES ($1::uuid, $2, $3)",
                               ceremony_id,
                               share.index,
                               share.digest);
            }
            const pqxx::result rows =
                tx.exec_params("UPDATE pki_recovery_ceremonies SET recovered_at = now() "
                               "WHERE id = $1::uuid RETURNING id::text, customer_id::text, "
                               "authority_id::text, threshold, share_count, recovered_at::text, "
                               "expires_at::text, created_at::text",
                               ceremony_id);
            return PkiRecoveryCeremonyRecord{rows[0][0].as<std::string>(),
                                             rows[0][1].as<std::string>(),
                                             rows[0][2].as<std::string>(),
                                             rows[0][3].as<int>(),
                                             rows[0][4].as<int>(),
                                             rows[0][5].as<std::string>(),
                                             rows[0][6].as<std::string>(),
                                             rows[0][7].as<std::string>()};
        });
    }

    Result<SecretMount> create_secret_mount(const NewSecretMount& input,
                                            const std::string& instance_kek) override {
        if (!valid_handle(input.name)) {
            return std::unexpected(
                StoreError{"invalid_input", "mount name must be a lowercase handle"});
        }
        return with_tx<SecretMount>([&](pqxx::work& tx) -> Result<SecretMount> {
            const pqxx::result scope = tx.exec_params(
                "SELECT organization_object.id::text FROM objects AS organization_object "
                "JOIN objects AS customer_object ON customer_object.id = "
                "organization_object.parent_id AND customer_object.object_type = 'customer' "
                "WHERE organization_object.object_type = 'organization' "
                "AND organization_object.external_id = $1 "
                "AND customer_object.external_id = $2",
                input.organization_id,
                input.customer_id);
            if (scope.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "organization_id is not inside the customer namespace"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO secrets_mounts ") +
                                   "(customer_id, organization_id, name, description) "
                                   "VALUES ($1::uuid, $2::uuid, $3, $4) RETURNING " +
                                   secret_mount_columns,
                               input.customer_id,
                               input.organization_id,
                               input.name,
                               input.description);
            const SecretMount mount = secret_mount_of(rows[0]);
            const std::string environment_external =
                "secrets/" + input.customer_id + "/" + input.organization_id;
            const pqxx::result environment =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('environment', $1, $2::uuid) "
                               "ON CONFLICT (object_type, external_id) DO UPDATE SET external_id = "
                               "EXCLUDED.external_id "
                               "RETURNING id::text",
                               environment_external,
                               scope[0][0].as<std::string>());
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('secret_mount', $1, $2::uuid)",
                           mount.id,
                           environment[0][0].as<std::string>());
            auto key = secrets_crypto::random_key();
            if (!key) {
                return std::unexpected(key.error());
            }
            auto stored =
                store_data_key_tx(tx, mount.customer_id, "mount", mount.id, 1, *key, instance_kek);
            secrets_crypto::cleanse(*key);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return mount;
        });
    }

    Result<std::vector<SecretMount>> list_secret_mounts(const std::string& customer_id,
                                                        const std::string& organization_id,
                                                        int limit) override {
        return with_tx<std::vector<SecretMount>>(
            [&](pqxx::work& tx) -> Result<std::vector<SecretMount>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + secret_mount_columns +
                        " FROM secrets_mounts WHERE "
                        "(NULLIF($1, '') IS NULL OR customer_id = NULLIF($1, '')::uuid) AND "
                        "(NULLIF($2, '') IS NULL OR organization_id = NULLIF($2, '')::uuid) "
                        "ORDER BY created_at DESC LIMIT $3",
                    customer_id,
                    organization_id,
                    limit);
                std::vector<SecretMount> output;
                for (const pqxx::row& row : rows) {
                    output.push_back(secret_mount_of(row));
                }
                return output;
            });
    }

    Result<SecretMount> get_secret_mount(const std::string& id) override {
        return with_tx<SecretMount>([&](pqxx::work& tx) { return secret_mount_tx(tx, id); });
    }

    Result<SecretVersion> put_secret(const PutSecret& input,
                                     const std::string& instance_kek) override {
        if (input.name.empty() || input.name.size() > 1024 || !input.value.is_object() ||
            !input.custom_metadata.is_object()) {
            return std::unexpected(StoreError{
                "invalid_input", "secret name, object value, and object metadata are required"});
        }
        return with_tx<SecretVersion>([&](pqxx::work& tx) -> Result<SecretVersion> {
            auto mount = secret_mount_tx(tx, input.mount_id);
            if (!mount) {
                return std::unexpected(mount.error());
            }
            pqxx::result entries = tx.exec_params(
                "SELECT id::text, current_version, custom_metadata::text FROM secrets_entries "
                "WHERE mount_id = $1::uuid AND name = $2 FOR UPDATE",
                input.mount_id,
                input.name);
            std::string entry_id;
            std::uint64_t current = 0;
            nlohmann::json metadata = input.custom_metadata;
            if (entries.empty()) {
                entries =
                    tx.exec_params("INSERT INTO secrets_entries (mount_id, name, custom_metadata) "
                                   "VALUES ($1::uuid, $2, $3::jsonb) "
                                   "RETURNING id::text, current_version, custom_metadata::text",
                                   input.mount_id,
                                   input.name,
                                   input.custom_metadata.dump());
                const pqxx::result mount_object = tx.exec_params(
                    "SELECT id::text FROM objects WHERE object_type = 'secret_mount' "
                    "AND external_id = $1",
                    input.mount_id);
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('secret', $1, $2::uuid)",
                               input.mount_id + "/" + input.name,
                               mount_object[0][0].as<std::string>());
            } else {
                current = entries[0][1].as<std::uint64_t>();
                metadata = input.custom_metadata.empty()
                               ? nlohmann::json::parse(entries[0][2].as<std::string>())
                               : input.custom_metadata;
            }
            entry_id = entries[0][0].as<std::string>();
            if (input.check_and_set != 0 && input.check_and_set != current) {
                return std::unexpected(StoreError{"conflict", "secret version changed"});
            }
            const std::uint64_t version = current + 1;
            auto key = data_key_tx(tx, "mount", mount->id, 1, instance_kek);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::string associated =
                "lake secret/" + mount->id + "/" + input.name + "/" + std::to_string(version);
            auto ciphertext = secrets_crypto::seal(*key, input.value.dump(), associated);
            secrets_crypto::cleanse(*key);
            if (!ciphertext) {
                return std::unexpected(ciphertext.error());
            }
            const pqxx::result created =
                tx.exec_params("INSERT INTO secrets_versions (entry_id, version, ciphertext) "
                               "VALUES ($1::uuid, $2, $3) RETURNING created_at::text",
                               entry_id,
                               version,
                               *ciphertext);
            const pqxx::result updated = tx.exec_params(
                "UPDATE secrets_entries SET current_version = $2, custom_metadata = $3::jsonb, "
                "updated_at = now() WHERE id = $1::uuid RETURNING updated_at::text",
                entry_id,
                version,
                metadata.dump());
            return SecretVersion{mount->id,
                                 input.name,
                                 version,
                                 input.value,
                                 metadata,
                                 created[0][0].as<std::string>(),
                                 "",
                                 ""};
        });
    }

    Result<SecretVersion> read_secret(const std::string& mount_id,
                                      const std::string& name,
                                      std::uint64_t version,
                                      const std::string& instance_kek) override {
        return with_tx<SecretVersion>([&](pqxx::work& tx) -> Result<SecretVersion> {
            auto mount = secret_mount_tx(tx, mount_id);
            if (!mount) {
                return std::unexpected(mount.error());
            }
            const pqxx::result rows = tx.exec_params(
                "SELECT versions.version, versions.ciphertext, versions.created_at::text, "
                "coalesce(versions.deleted_at::text,''), "
                "coalesce(versions.destroyed_at::text,''), entries.custom_metadata::text "
                "FROM secrets_entries AS entries JOIN secrets_versions AS versions "
                "ON versions.entry_id = entries.id WHERE entries.mount_id = $1::uuid "
                "AND entries.name = $2 AND versions.version = "
                "CASE WHEN $3 = 0 THEN entries.current_version ELSE $3 END",
                mount_id,
                name,
                version);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "secret version does not exist"});
            }
            if (!rows[0][4].as<std::string>().empty()) {
                return std::unexpected(StoreError{"not_found", "secret version was destroyed"});
            }
            if (!rows[0][3].as<std::string>().empty()) {
                return std::unexpected(StoreError{"not_found", "secret version is deleted"});
            }
            auto key = data_key_tx(tx, "mount", mount_id, 1, instance_kek);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::uint64_t selected = rows[0][0].as<std::uint64_t>();
            const std::string associated =
                "lake secret/" + mount_id + "/" + name + "/" + std::to_string(selected);
            auto plaintext = secrets_crypto::open(*key, rows[0][1].as<std::string>(), associated);
            secrets_crypto::cleanse(*key);
            if (!plaintext) {
                return std::unexpected(plaintext.error());
            }
            nlohmann::json value = nlohmann::json::parse(*plaintext, nullptr, false);
            if (value.is_discarded() || !value.is_object()) {
                return std::unexpected(
                    StoreError{"store_error", "stored secret is not valid JSON"});
            }
            return SecretVersion{mount_id,
                                 name,
                                 selected,
                                 std::move(value),
                                 nlohmann::json::parse(rows[0][5].as<std::string>()),
                                 rows[0][2].as<std::string>(),
                                 "",
                                 ""};
        });
    }

    Result<SecretMetadata> secret_metadata(const std::string& mount_id,
                                           const std::string& name) override {
        return with_tx<SecretMetadata>(
            [&](pqxx::work& tx) { return secret_metadata_tx(tx, mount_id, name); });
    }

    Result<SecretMetadata> change_secret_versions(const std::string& mount_id,
                                                  const std::string& name,
                                                  const std::vector<std::uint64_t>& versions,
                                                  SecretVersionAction action) override {
        if (versions.empty()) {
            return std::unexpected(StoreError{"invalid_input", "at least one version is required"});
        }
        return with_tx<SecretMetadata>([&](pqxx::work& tx) -> Result<SecretMetadata> {
            const pqxx::result entries = tx.exec_params(
                "SELECT id::text FROM secrets_entries WHERE mount_id = $1::uuid AND name = $2 "
                "FOR UPDATE",
                mount_id,
                name);
            if (entries.empty()) {
                return std::unexpected(StoreError{"not_found", "secret does not exist"});
            }
            const std::string entry_id = entries[0][0].as<std::string>();
            for (const std::uint64_t version : versions) {
                const pqxx::result row =
                    tx.exec_params("SELECT destroyed_at IS NOT NULL FROM secrets_versions "
                                   "WHERE entry_id = $1::uuid AND version = $2 FOR UPDATE",
                                   entry_id,
                                   version);
                if (row.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "secret version does not exist"});
                }
                if (action == SecretVersionAction::Undelete && row[0][0].as<bool>()) {
                    return std::unexpected(
                        StoreError{"conflict", "a destroyed secret version cannot be restored"});
                }
            }
            for (const std::uint64_t version : versions) {
                if (action == SecretVersionAction::Delete) {
                    tx.exec_params(
                        "UPDATE secrets_versions SET deleted_at = COALESCE(deleted_at, now()) "
                        "WHERE entry_id = $1::uuid AND version = $2 "
                        "AND destroyed_at IS NULL",
                        entry_id,
                        version);
                } else if (action == SecretVersionAction::Undelete) {
                    tx.exec_params("UPDATE secrets_versions SET deleted_at = NULL "
                                   "WHERE entry_id = $1::uuid AND version = $2",
                                   entry_id,
                                   version);
                } else {
                    tx.exec_params("UPDATE secrets_versions SET ciphertext = NULL, "
                                   "deleted_at = COALESCE(deleted_at, now()), destroyed_at = now() "
                                   "WHERE entry_id = $1::uuid AND version = $2",
                                   entry_id,
                                   version);
                }
            }
            tx.exec_params("UPDATE secrets_entries SET updated_at = now() WHERE id = $1::uuid",
                           entry_id);
            return secret_metadata_tx(tx, mount_id, name);
        });
    }

    Result<IssuedDynamicSecret> lease_dynamic_secret(const NewDynamicSecretLease& input,
                                                     const std::string& instance_kek) override {
        if (input.role.empty() || !input.value.is_object() || input.lifetime_seconds < 1 ||
            input.lifetime_seconds > 3600) {
            return std::unexpected(StoreError{"invalid_input", "dynamic-secret lease is invalid"});
        }
        return with_tx<IssuedDynamicSecret>([&](pqxx::work& tx) -> Result<IssuedDynamicSecret> {
            const std::string id = new_uuid_v4();
            auto key = data_key_tx(tx, "mount", input.mount_id, 1, instance_kek);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::string associated = "lake dynamic secret/" + input.mount_id + "/" + id;
            const auto encrypted = secrets_crypto::seal(*key, input.value.dump(), associated);
            secrets_crypto::cleanse(*key);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO secrets_dynamic_leases "
                "(id, mount_id, role, encrypted_value, expires_at) "
                "VALUES ($1::uuid, $2::uuid, $3, $4, "
                "now() + make_interval(secs => $5)) RETURNING id::text, mount_id::text, role, "
                "expires_at::text, coalesce(revoked_at::text,''), renewals, created_at::text",
                id,
                input.mount_id,
                input.role,
                *encrypted,
                input.lifetime_seconds);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "secret mount does not exist"});
            }
            DynamicSecretLease lease{rows[0][0].as<std::string>(),
                                     rows[0][1].as<std::string>(),
                                     rows[0][2].as<std::string>(),
                                     rows[0][3].as<std::string>(),
                                     rows[0][4].as<std::string>(),
                                     rows[0][5].as<int>(),
                                     rows[0][6].as<std::string>()};
            return IssuedDynamicSecret{std::move(lease), input.value};
        });
    }

    Result<DynamicSecretLease> get_dynamic_secret_lease(const std::string& id) override {
        return with_tx<DynamicSecretLease>(
            [&](pqxx::work& tx) { return dynamic_secret_lease_tx(tx, id); });
    }

    Result<DynamicSecretLease> renew_dynamic_secret_lease(const std::string& id,
                                                          int lifetime_seconds) override {
        if (lifetime_seconds < 1 || lifetime_seconds > 3600) {
            return std::unexpected(
                StoreError{"invalid_input", "lease lifetime must be at most one hour"});
        }
        return with_tx<DynamicSecretLease>([&](pqxx::work& tx) -> Result<DynamicSecretLease> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE secrets_dynamic_leases SET expires_at = now() + make_interval(secs => $2), "
                "renewals = renewals + 1 WHERE id = $1::uuid AND revoked_at IS NULL "
                "AND expires_at > now() RETURNING id::text",
                id,
                lifetime_seconds);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "dynamic-secret lease is not active"});
            }
            return dynamic_secret_lease_tx(tx, id);
        });
    }

    Result<DynamicSecretLease> revoke_dynamic_secret_lease(const std::string& id) override {
        return with_tx<DynamicSecretLease>([&](pqxx::work& tx) -> Result<DynamicSecretLease> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE secrets_dynamic_leases SET revoked_at = coalesce(revoked_at, now()) "
                "WHERE id = $1::uuid RETURNING id::text",
                id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "dynamic-secret lease does not exist"});
            }
            return dynamic_secret_lease_tx(tx, id);
        });
    }

    Result<EncryptionKey> create_encryption_key(const NewEncryptionKey& input,
                                                const std::string& instance_kek) override {
        if (!valid_handle(input.name)) {
            return std::unexpected(
                StoreError{"invalid_input", "encryption key name must be a lowercase handle"});
        }
        return with_tx<EncryptionKey>([&](pqxx::work& tx) -> Result<EncryptionKey> {
            auto mount = secret_mount_tx(tx, input.mount_id);
            if (!mount) {
                return std::unexpected(mount.error());
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO secrets_encryption_keys (mount_id, name) ") +
                    "VALUES ($1::uuid, $2) RETURNING " + encryption_key_columns,
                input.mount_id,
                input.name);
            const EncryptionKey key = encryption_key_of(rows[0]);
            auto material = secrets_crypto::random_key();
            if (!material) {
                return std::unexpected(material.error());
            }
            auto stored = store_data_key_tx(
                tx, mount->customer_id, "encryption", key.id, 1, *material, instance_kek);
            secrets_crypto::cleanse(*material);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return key;
        });
    }

    Result<std::vector<EncryptionKey>> list_encryption_keys(const std::string& mount_id,
                                                            int limit) override {
        return with_tx<std::vector<EncryptionKey>>(
            [&](pqxx::work& tx) -> Result<std::vector<EncryptionKey>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + encryption_key_columns +
                                       " FROM secrets_encryption_keys WHERE mount_id = $1::uuid "
                                       "ORDER BY created_at DESC LIMIT $2",
                                   mount_id,
                                   limit);
                std::vector<EncryptionKey> output;
                for (const pqxx::row& row : rows) {
                    output.push_back(encryption_key_of(row));
                }
                return output;
            });
    }

    Result<EncryptionKey> get_encryption_key(const std::string& mount_id,
                                             const std::string& name) override {
        return with_tx<EncryptionKey>(
            [&](pqxx::work& tx) { return encryption_key_tx(tx, mount_id, name); });
    }

    Result<EncryptionKey> rotate_encryption_key(const std::string& mount_id,
                                                const std::string& name,
                                                const std::string& instance_kek) override {
        return with_tx<EncryptionKey>([&](pqxx::work& tx) -> Result<EncryptionKey> {
            auto mount = secret_mount_tx(tx, mount_id);
            auto key = encryption_key_tx(tx, mount_id, name);
            if (!mount || !key) {
                return std::unexpected(!mount ? mount.error() : key.error());
            }
            const pqxx::result locked =
                tx.exec_params("SELECT current_version FROM secrets_encryption_keys WHERE id = "
                               "$1::uuid FOR UPDATE",
                               key->id);
            const std::uint64_t version = locked[0][0].as<std::uint64_t>() + 1;
            auto material = secrets_crypto::random_key();
            if (!material) {
                return std::unexpected(material.error());
            }
            auto stored = store_data_key_tx(
                tx, mount->customer_id, "encryption", key->id, version, *material, instance_kek);
            secrets_crypto::cleanse(*material);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE secrets_encryption_keys SET current_version = $2, ") +
                    "updated_at = now() WHERE id = $1::uuid RETURNING " + encryption_key_columns,
                key->id,
                version);
            return encryption_key_of(rows[0]);
        });
    }

    Result<TransitValue> encrypt(const std::string& mount_id,
                                 const std::string& name,
                                 const std::string& plaintext,
                                 const std::string& instance_kek) override {
        return with_tx<TransitValue>([&](pqxx::work& tx) -> Result<TransitValue> {
            auto mount = secret_mount_tx(tx, mount_id);
            auto key = encryption_key_tx(tx, mount_id, name);
            if (!mount || !key) {
                return std::unexpected(!mount ? mount.error() : key.error());
            }
            return encrypt_tx(tx, *mount, *key, plaintext, instance_kek);
        });
    }

    Result<TransitValue> decrypt(const std::string& mount_id,
                                 const std::string& name,
                                 const std::string& ciphertext,
                                 const std::string& instance_kek) override {
        return with_tx<TransitValue>([&](pqxx::work& tx) -> Result<TransitValue> {
            auto key = encryption_key_tx(tx, mount_id, name);
            auto parsed = secrets_crypto::parse_ciphertext(ciphertext);
            if (!key) {
                return std::unexpected(key.error());
            }
            if (!parsed || parsed->key_id != key->id) {
                return std::unexpected(
                    parsed ? StoreError{"invalid_input", "ciphertext names another key"}
                           : parsed.error());
            }
            auto material = data_key_tx(tx, "encryption", key->id, parsed->version, instance_kek);
            if (!material) {
                return std::unexpected(material.error());
            }
            auto encryption_key = secrets_crypto::derive(*material, "lake transit encryption v1");
            secrets_crypto::cleanse(*material);
            if (!encryption_key) {
                return std::unexpected(encryption_key.error());
            }
            const std::string associated =
                "lake transit/" + key->id + "/" + std::to_string(parsed->version);
            auto plaintext = secrets_crypto::open(*encryption_key, parsed->sealed, associated);
            secrets_crypto::cleanse(*encryption_key);
            if (!plaintext) {
                return std::unexpected(plaintext.error());
            }
            auto encoded = secrets_crypto::base64url_encode(*plaintext);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            return TransitValue{*encoded, parsed->version, "base64url"};
        });
    }

    Result<TransitValue> rewrap(const std::string& mount_id,
                                const std::string& name,
                                const std::string& ciphertext,
                                const std::string& instance_kek) override {
        return with_tx<TransitValue>([&](pqxx::work& tx) -> Result<TransitValue> {
            auto mount = secret_mount_tx(tx, mount_id);
            auto key = encryption_key_tx(tx, mount_id, name);
            auto parsed = secrets_crypto::parse_ciphertext(ciphertext);
            if (!mount || !key) {
                return std::unexpected(!mount ? mount.error() : key.error());
            }
            if (!parsed || parsed->key_id != key->id) {
                return std::unexpected(
                    parsed ? StoreError{"invalid_input", "ciphertext names another key"}
                           : parsed.error());
            }
            auto old_material =
                data_key_tx(tx, "encryption", key->id, parsed->version, instance_kek);
            if (!old_material) {
                return std::unexpected(old_material.error());
            }
            auto old_key = secrets_crypto::derive(*old_material, "lake transit encryption v1");
            secrets_crypto::cleanse(*old_material);
            if (!old_key) {
                return std::unexpected(old_key.error());
            }
            const std::string associated =
                "lake transit/" + key->id + "/" + std::to_string(parsed->version);
            auto plaintext = secrets_crypto::open(*old_key, parsed->sealed, associated);
            secrets_crypto::cleanse(*old_key);
            if (!plaintext) {
                return std::unexpected(plaintext.error());
            }
            auto encoded = secrets_crypto::base64url_encode(*plaintext);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            return encrypt_tx(tx, *mount, *key, *encoded, instance_kek);
        });
    }

    Result<TransitValue> sign(const std::string& mount_id,
                              const std::string& name,
                              const std::string& message,
                              std::uint64_t version,
                              const std::string& instance_kek) override {
        return with_tx<TransitValue>([&](pqxx::work& tx) -> Result<TransitValue> {
            auto key = encryption_key_tx(tx, mount_id, name);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::uint64_t selected = version == 0 ? key->current_version : version;
            auto decoded = secrets_crypto::base64url_decode(message);
            auto material = data_key_tx(tx, "encryption", key->id, selected, instance_kek);
            if (!decoded || !material) {
                return std::unexpected(!decoded ? decoded.error() : material.error());
            }
            auto signing_key = secrets_crypto::derive(*material, "lake transit signing v1");
            secrets_crypto::cleanse(*material);
            if (!signing_key) {
                return std::unexpected(signing_key.error());
            }
            auto signature = secrets_crypto::sign(*signing_key, *decoded);
            secrets_crypto::cleanse(*signing_key);
            if (!signature) {
                return std::unexpected(signature.error());
            }
            return TransitValue{*signature, selected, "ed25519"};
        });
    }

    Result<TransitVerification> verify(const std::string& mount_id,
                                       const std::string& name,
                                       const std::string& message,
                                       const std::string& signature,
                                       std::uint64_t version,
                                       const std::string& instance_kek) override {
        return with_tx<TransitVerification>([&](pqxx::work& tx) -> Result<TransitVerification> {
            auto key = encryption_key_tx(tx, mount_id, name);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::uint64_t selected = version == 0 ? key->current_version : version;
            auto decoded = secrets_crypto::base64url_decode(message);
            auto material = data_key_tx(tx, "encryption", key->id, selected, instance_kek);
            if (!decoded || !material) {
                return std::unexpected(!decoded ? decoded.error() : material.error());
            }
            auto signing_key = secrets_crypto::derive(*material, "lake transit signing v1");
            secrets_crypto::cleanse(*material);
            if (!signing_key) {
                return std::unexpected(signing_key.error());
            }
            auto valid = secrets_crypto::verify(*signing_key, *decoded, signature);
            secrets_crypto::cleanse(*signing_key);
            if (!valid) {
                return std::unexpected(valid.error());
            }
            return TransitVerification{*valid, selected, "ed25519"};
        });
    }

    Result<TransitValue> hmac(const std::string& mount_id,
                              const std::string& name,
                              const std::string& message,
                              std::uint64_t version,
                              const std::string& instance_kek) override {
        return with_tx<TransitValue>([&](pqxx::work& tx) -> Result<TransitValue> {
            auto key = encryption_key_tx(tx, mount_id, name);
            if (!key) {
                return std::unexpected(key.error());
            }
            const std::uint64_t selected = version == 0 ? key->current_version : version;
            auto decoded = secrets_crypto::base64url_decode(message);
            auto material = data_key_tx(tx, "encryption", key->id, selected, instance_kek);
            if (!decoded || !material) {
                return std::unexpected(!decoded ? decoded.error() : material.error());
            }
            auto hmac_key = secrets_crypto::derive(*material, "lake transit hmac v1");
            secrets_crypto::cleanse(*material);
            if (!hmac_key) {
                return std::unexpected(hmac_key.error());
            }
            auto digest = secrets_crypto::hmac(*hmac_key, *decoded);
            secrets_crypto::cleanse(*hmac_key);
            if (!digest) {
                return std::unexpected(digest.error());
            }
            return TransitValue{*digest, selected, "hmac-sha256"};
        });
    }

    Result<DerivedEncryptionKey> derive_encryption_key(const std::string& mount_id,
                                                       const std::string& parent_name,
                                                       const std::string& name,
                                                       const std::string& context,
                                                       const std::string& instance_kek) override {
        if (!valid_handle(name)) {
            return std::unexpected(
                StoreError{"invalid_input", "derived key name must be a lowercase handle"});
        }
        return with_tx<DerivedEncryptionKey>([&](pqxx::work& tx) -> Result<DerivedEncryptionKey> {
            auto mount = secret_mount_tx(tx, mount_id);
            auto parent = encryption_key_tx(tx, mount_id, parent_name);
            if (!mount || !parent) {
                return std::unexpected(!mount ? mount.error() : parent.error());
            }
            auto decoded_context = secrets_crypto::base64url_decode(context);
            auto material =
                data_key_tx(tx, "encryption", parent->id, parent->current_version, instance_kek);
            if (!decoded_context || !material) {
                return std::unexpected(!decoded_context ? decoded_context.error()
                                                        : material.error());
            }
            auto derived =
                secrets_crypto::derive(*material, "lake transit child v1/" + *decoded_context);
            secrets_crypto::cleanse(*material);
            if (!derived) {
                return std::unexpected(derived.error());
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO secrets_encryption_keys ") +
                    "(mount_id, name, derived_from_id) VALUES ($1::uuid, $2, $3::uuid) "
                    "RETURNING " +
                    encryption_key_columns,
                mount_id,
                name,
                parent->id);
            const EncryptionKey child = encryption_key_of(rows[0]);
            auto stored = store_data_key_tx(
                tx, mount->customer_id, "encryption", child.id, 1, *derived, instance_kek);
            secrets_crypto::cleanse(*derived);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            return DerivedEncryptionKey{child, parent->current_version};
        });
    }

    Result<CustomerKekRotation> rotate_customer_kek(const std::string& customer_id,
                                                    const std::string& instance_kek) override {
        return with_tx<CustomerKekRotation>([&](pqxx::work& tx) -> Result<CustomerKekRotation> {
            auto root = secrets_crypto::instance_key(instance_kek);
            auto old_key = customer_kek_tx(tx, customer_id, instance_kek, false);
            if (!root || !old_key) {
                return std::unexpected(!root ? root.error() : old_key.error());
            }
            const pqxx::result keystore = tx.exec_params(
                "SELECT version FROM keystore WHERE customer_id = $1::uuid FOR UPDATE",
                customer_id);
            const std::uint64_t version = keystore[0][0].as<std::uint64_t>() + 1;
            auto new_key = secrets_crypto::random_key();
            if (!new_key) {
                secrets_crypto::cleanse(*root);
                secrets_crypto::cleanse(*old_key);
                return std::unexpected(new_key.error());
            }
            const pqxx::result data_keys =
                tx.exec_params("SELECT owner_kind, owner_id::text, key_version, wrapped_key "
                               "FROM secrets_data_keys "
                               "WHERE customer_id = $1::uuid FOR UPDATE",
                               customer_id);
            for (const pqxx::row& row : data_keys) {
                const std::string owner_kind = row[0].as<std::string>();
                const std::string owner_id = row[1].as<std::string>();
                const std::uint64_t key_version = row[2].as<std::uint64_t>();
                const std::string associated = "lake data key/" + customer_id + "/" + owner_id +
                                               "/" + std::to_string(key_version);
                auto raw = secrets_crypto::open(*old_key, row[3].as<std::string>(), associated);
                if (!raw) {
                    secrets_crypto::cleanse(*root);
                    secrets_crypto::cleanse(*old_key);
                    secrets_crypto::cleanse(*new_key);
                    return std::unexpected(raw.error());
                }
                auto wrapped = secrets_crypto::seal(*new_key, *raw, associated);
                if (!wrapped) {
                    secrets_crypto::cleanse(*root);
                    secrets_crypto::cleanse(*old_key);
                    secrets_crypto::cleanse(*new_key);
                    return std::unexpected(wrapped.error());
                }
                tx.exec_params("UPDATE secrets_data_keys SET wrapped_key = $4 "
                               "WHERE owner_kind = $1 AND owner_id = $2::uuid "
                               "AND key_version = $3",
                               owner_kind,
                               owner_id,
                               key_version,
                               *wrapped);
            }
            const pqxx::result authorities =
                tx.exec_params("SELECT id::text, root_key_ciphertext, intermediate_ciphertext "
                               "FROM pki_authorities WHERE customer_id = $1::uuid FOR UPDATE",
                               customer_id);
            for (const pqxx::row& row : authorities) {
                const std::string root_associated = "lake pki authority/" + customer_id + "/root";
                const std::string intermediate_associated =
                    "lake pki authority/" + customer_id + "/intermediate";
                auto root =
                    secrets_crypto::open(*old_key, row[1].as<std::string>(), root_associated);
                auto intermediate = secrets_crypto::open(
                    *old_key, row[2].as<std::string>(), intermediate_associated);
                if (!root || !intermediate) {
                    secrets_crypto::cleanse(*old_key);
                    secrets_crypto::cleanse(*new_key);
                    return std::unexpected(!root ? root.error() : intermediate.error());
                }
                auto wrapped_root = secrets_crypto::seal(*new_key, *root, root_associated);
                auto wrapped_intermediate =
                    secrets_crypto::seal(*new_key, *intermediate, intermediate_associated);
                if (!wrapped_root || !wrapped_intermediate) {
                    secrets_crypto::cleanse(*old_key);
                    secrets_crypto::cleanse(*new_key);
                    return std::unexpected(!wrapped_root ? wrapped_root.error()
                                                         : wrapped_intermediate.error());
                }
                tx.exec_params("UPDATE pki_authorities SET root_key_ciphertext = $2, "
                               "intermediate_ciphertext = $3 WHERE id = $1::uuid",
                               row[0].as<std::string>(),
                               *wrapped_root,
                               *wrapped_intermediate);
            }
            secrets_crypto::cleanse(*old_key);
            const std::string raw_new(reinterpret_cast<const char*>(new_key->data()),
                                      new_key->size());
            const std::string associated =
                "lake customer kek/" + customer_id + "/" + std::to_string(version);
            auto wrapped_customer = secrets_crypto::seal(*root, raw_new, associated);
            secrets_crypto::cleanse(*root);
            secrets_crypto::cleanse(*new_key);
            if (!wrapped_customer) {
                return std::unexpected(wrapped_customer.error());
            }
            tx.exec_params("UPDATE keystore SET wrapped_key = convert_to($2, 'UTF8'), "
                           "version = $3, updated_at = now() WHERE customer_id = $1::uuid",
                           customer_id,
                           *wrapped_customer,
                           version);
            return CustomerKekRotation{
                customer_id,
                version,
                static_cast<std::uint64_t>(data_keys.size() + authorities.size())};
        });
    }

    Result<OfflineGrant> issue_offline_grant(const NewOfflineGrant& input) override {
        if (input.id.empty() || input.user_id.empty() || input.principal_taap_id.empty() ||
            input.device_id.empty() || input.scopes.empty() || input.duration_seconds <= 0 ||
            input.duration_seconds > 604800 || !input.credential.is_object()) {
            return std::unexpected(StoreError{"invalid_input", "offline grant is incomplete"});
        }
        return with_tx<OfflineGrant>([&](pqxx::work& tx) -> Result<OfflineGrant> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO offline_recovery_grants ") +
                    "(id, user_id, principal_taap_id, device_id, scopes, credential, expires_at) "
                    "SELECT $1::uuid, $2::uuid, $3, $4::uuid, "
                    "ARRAY(SELECT jsonb_array_elements_text($5::jsonb)), $6::jsonb, "
                    "now() + make_interval(secs => $7) FROM devices "
                    "WHERE id = $4::uuid AND user_id = $2::uuid AND revoked_at IS NULL RETURNING " +
                    offline_grant_columns,
                input.id,
                input.user_id,
                input.principal_taap_id,
                input.device_id,
                nlohmann::json(input.scopes).dump(),
                input.credential.dump(),
                input.duration_seconds);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "active device not found"});
            }
            return offline_grant_of(rows[0]);
        });
    }

    Result<std::vector<OfflineGrant>> list_offline_grants(const std::string& user_id) override {
        return with_tx<std::vector<OfflineGrant>>(
            [&](pqxx::work& tx) -> Result<std::vector<OfflineGrant>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + offline_grant_columns +
                                       " FROM offline_recovery_grants WHERE user_id = $1::uuid "
                                       "ORDER BY issued_at DESC",
                                   user_id);
                std::vector<OfflineGrant> output;
                for (const pqxx::row& row : rows) {
                    output.push_back(offline_grant_of(row));
                }
                return output;
            });
    }

    Result<OfflineGrant> get_offline_grant(const std::string& id) override {
        return with_tx<OfflineGrant>([&](pqxx::work& tx) -> Result<OfflineGrant> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + offline_grant_columns +
                                   " FROM offline_recovery_grants WHERE id = $1::uuid",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "offline grant not found"});
            }
            return offline_grant_of(rows[0]);
        });
    }

    Result<OfflineGrant> revoke_offline_grant(const std::string& user_id,
                                              const std::string& id) override {
        return with_tx<OfflineGrant>([&](pqxx::work& tx) -> Result<OfflineGrant> {
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE offline_recovery_grants SET revoked_at = ") +
                    "coalesce(revoked_at, now()) WHERE id = $1::uuid AND user_id = $2::uuid "
                    "RETURNING " +
                    offline_grant_columns,
                id,
                user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "offline grant not found"});
            }
            return offline_grant_of(rows[0]);
        });
    }

    Result<IssuedSession> recover_with_code(const std::string& pool_id,
                                            const std::string& email,
                                            const std::string& code,
                                            const std::string& device_name) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result user_rows = tx.exec_params(
                "SELECT user_id::text FROM credentials WHERE pool_id = $1 AND kind = 'password' "
                "AND subject = lower($2) AND revoked_at IS NULL",
                pool_id,
                email);
            if (user_rows.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "invalid email or recovery code"});
            }
            const std::string user_id = user_rows[0][0].as<std::string>();
            const pqxx::result recovery_rows = tx.exec_params(
                "SELECT id::text, secret_hash FROM credentials WHERE user_id = $1::uuid "
                "AND pool_id = $2 AND kind = 'recovery' AND revoked_at IS NULL FOR UPDATE",
                user_id,
                pool_id);
            const auto recovery = std::ranges::find_if(recovery_rows, [&](const pqxx::row& row) {
                return identity_crypto::verify_secret(row[1].as<std::string>(), code);
            });
            if (recovery == recovery_rows.end()) {
                return std::unexpected(
                    StoreError{"unauthorized", "invalid email or recovery code"});
            }
            tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE id = $1::uuid",
                           (*recovery)[0].as<std::string>());
            return issue_session(
                tx, user_id, pool_id, device_name, "browser", "", {"*"}, "", 86400);
        });
    }

    Result<RecoveryAttempt> begin_email_recovery(const std::string& pool_id,
                                                 const std::string& email,
                                                 int cooling_seconds) override {
        if (cooling_seconds < 3600) {
            return std::unexpected(
                StoreError{"invalid_input", "email recovery cooling period is too short"});
        }
        const std::string token = identity_crypto::random_secret();
        const std::string token_hash = identity_crypto::hash_secret(token);
        if (token.empty() || token_hash.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create recovery token"});
        }
        return with_tx<RecoveryAttempt>([&](pqxx::work& tx) -> Result<RecoveryAttempt> {
            const pqxx::result users = tx.exec_params(
                "SELECT user_id::text FROM credentials WHERE pool_id = $1 AND kind = 'password' "
                "AND subject = lower($2) AND revoked_at IS NULL",
                pool_id,
                email);
            if (users.empty()) {
                return std::unexpected(StoreError{"not_found", "account not found"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO offline_recovery_attempts ") +
                    "(user_id, method, status, token_hash, ready_at, expires_at) "
                    "VALUES ($1::uuid, 'email', 'cooling', $2, "
                    "now() + make_interval(secs => $3), "
                    "now() + make_interval(secs => $3) + interval '24 hours') RETURNING " +
                    recovery_attempt_columns,
                users[0][0].as<std::string>(),
                token_hash,
                cooling_seconds);
            const RecoveryAttempt attempt = recovery_attempt_of(rows[0]);
            tx.exec_params("INSERT INTO offline_recovery_deliveries "
                           "(recovery_id, email, token, kind) "
                           "VALUES ($1::uuid, $2, $3, 'email_recovery_started')",
                           attempt.id,
                           email,
                           token);
            notify_devices(tx, attempt, "email_recovery_started");
            return attempt;
        });
    }

    Result<RecoveryAttempt> begin_admin_recovery(const std::string& organization_id,
                                                 const std::string& user_id,
                                                 const std::string& requested_by) override {
        return with_tx<RecoveryAttempt>([&](pqxx::work& tx) -> Result<RecoveryAttempt> {
            if (!organization_admin(tx, organization_id, requested_by)) {
                return std::unexpected(
                    StoreError{"unauthorized", "requester is not an organization admin"});
            }
            const pqxx::result target =
                tx.exec_params("SELECT 1 FROM memberships WHERE organization_id = $1::uuid "
                               "AND principal_type = 'user' AND principal_id = $2",
                               organization_id,
                               user_id);
            if (target.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "target user is not in the organization"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO offline_recovery_attempts ") +
                    "(user_id, organization_id, method, status, ready_at, expires_at) "
                    "VALUES ($1::uuid, $2::uuid, 'admin', 'pending_approval', now(), "
                    "now() + interval '24 hours') RETURNING " +
                    recovery_attempt_columns,
                user_id,
                organization_id);
            const RecoveryAttempt attempt = recovery_attempt_of(rows[0]);
            tx.exec_params("INSERT INTO offline_recovery_admin_approvals "
                           "(recovery_id, approver_taap_id) VALUES ($1::uuid, $2)",
                           attempt.id,
                           requested_by);
            notify_devices(tx, attempt, "admin_recovery_requested");
            RecoveryAttempt output = attempt;
            output.approval_count = 1;
            return output;
        });
    }

    Result<RecoveryAttempt> get_recovery_attempt(const std::string& recovery_id) override {
        return with_tx<RecoveryAttempt>([&](pqxx::work& tx) -> Result<RecoveryAttempt> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + recovery_attempt_columns +
                                   " FROM offline_recovery_attempts WHERE id = $1::uuid",
                               recovery_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "recovery attempt not found"});
            }
            return recovery_attempt_of(rows[0]);
        });
    }

    Result<RecoveryAttempt> approve_admin_recovery(const std::string& recovery_id,
                                                   const std::string& approved_by) override {
        return with_tx<RecoveryAttempt>([&](pqxx::work& tx) -> Result<RecoveryAttempt> {
            const pqxx::result attempts = tx.exec_params(
                std::string("SELECT ") + recovery_attempt_columns +
                    " FROM offline_recovery_attempts WHERE id = $1::uuid AND method = 'admin' "
                    "FOR UPDATE",
                recovery_id);
            if (attempts.empty()) {
                return std::unexpected(StoreError{"not_found", "admin recovery not found"});
            }
            RecoveryAttempt attempt = recovery_attempt_of(attempts[0]);
            if (!organization_admin(tx, attempt.organization_id, approved_by)) {
                return std::unexpected(
                    StoreError{"unauthorized", "approver is not an organization admin"});
            }
            const pqxx::result duplicate =
                tx.exec_params("SELECT 1 FROM offline_recovery_admin_approvals "
                               "WHERE recovery_id = $1::uuid AND approver_taap_id = $2",
                               recovery_id,
                               approved_by);
            if (!duplicate.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "a different organization admin must approve recovery"});
            }
            const pqxx::result valid =
                tx.exec_params("SELECT 1 FROM offline_recovery_attempts WHERE id = $1::uuid "
                               "AND expires_at > now() AND status = 'pending_approval'",
                               recovery_id);
            if (valid.empty()) {
                return std::unexpected(StoreError{"conflict", "admin recovery is not pending"});
            }
            tx.exec_params("INSERT INTO offline_recovery_admin_approvals "
                           "(recovery_id, approver_taap_id) VALUES ($1::uuid, $2)",
                           recovery_id,
                           approved_by);
            const std::string token = identity_crypto::random_secret();
            const std::string token_hash = identity_crypto::hash_secret(token);
            if (token.empty() || token_hash.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "could not create recovery token"});
            }
            const pqxx::result updated = tx.exec_params(
                std::string("UPDATE offline_recovery_attempts SET status = 'approved', ") +
                    "token_hash = $2 WHERE id = $1::uuid RETURNING " + recovery_attempt_columns,
                recovery_id,
                token_hash);
            attempt = recovery_attempt_of(updated[0]);
            const pqxx::result user =
                tx.exec_params("SELECT email FROM users WHERE id = $1::uuid", attempt.user_id);
            tx.exec_params("INSERT INTO offline_recovery_deliveries "
                           "(recovery_id, email, token, kind) "
                           "VALUES ($1::uuid, $2, $3, 'admin_recovery_approved')",
                           recovery_id,
                           user[0][0].as<std::string>(),
                           token);
            notify_devices(tx, attempt, "admin_recovery_approved");
            return attempt;
        });
    }

    Result<IssuedSession> complete_recovery(const std::string& recovery_id,
                                            const std::string& token,
                                            const std::string& device_name) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result attempts = tx.exec_params(
                "SELECT user_id::text, method, status, token_hash, ready_at <= now(), "
                "expires_at > now() FROM offline_recovery_attempts WHERE id = $1::uuid FOR UPDATE",
                recovery_id);
            if (attempts.empty() || attempts[0][3].is_null() ||
                !identity_crypto::verify_secret(attempts[0][3].as<std::string>(), token)) {
                return std::unexpected(StoreError{"unauthorized", "invalid recovery token"});
            }
            if (!attempts[0][4].as<bool>()) {
                return std::unexpected(
                    StoreError{"conflict", "recovery cooling period has not ended"});
            }
            if (!attempts[0][5].as<bool>()) {
                return std::unexpected(StoreError{"conflict", "recovery token has expired"});
            }
            const std::string method = attempts[0][1].as<std::string>();
            const std::string status = attempts[0][2].as<std::string>();
            if (status == "completed" || (method == "admin" && status != "approved")) {
                return std::unexpected(StoreError{"conflict", "recovery is not completable"});
            }
            const std::string user_id = attempts[0][0].as<std::string>();
            tx.exec_params("UPDATE sessions SET revoked_at = coalesce(revoked_at, now()) "
                           "WHERE user_id = $1::uuid",
                           user_id);
            tx.exec_params("UPDATE offline_recovery_attempts SET status = 'completed', "
                           "completed_at = now(), token_hash = NULL WHERE id = $1::uuid",
                           recovery_id);
            const pqxx::result pool = tx.exec_params(
                "SELECT pool_id FROM credentials WHERE user_id = $1::uuid AND kind = 'password' "
                "AND revoked_at IS NULL LIMIT 1",
                user_id);
            if (pool.empty()) {
                return std::unexpected(StoreError{"not_found", "password credential not found"});
            }
            const RecoveryAttempt notification{
                recovery_id, user_id, {}, method, "completed", {}, {}, {}, 0};
            notify_devices(tx, notification, "account_recovery_completed");
            return issue_session(tx,
                                 user_id,
                                 pool[0][0].as<std::string>(),
                                 device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 86400);
        });
    }

    Result<std::vector<RecoveryDelivery>> claim_recovery_deliveries(int limit) override {
        return with_tx<std::vector<RecoveryDelivery>>(
            [&](pqxx::work& tx) -> Result<std::vector<RecoveryDelivery>> {
                const pqxx::result rows = tx.exec_params(
                    "WITH selected AS (SELECT id, recovery_id, email, token, kind, created_at "
                    "FROM offline_recovery_deliveries WHERE delivered_at IS NULL "
                    "ORDER BY created_at FOR UPDATE SKIP LOCKED LIMIT $1) "
                    "UPDATE offline_recovery_deliveries AS delivery SET delivered_at = now(), "
                    "token = NULL FROM selected WHERE delivery.id = selected.id RETURNING "
                    "delivery.id::text, selected.recovery_id::text, selected.email, "
                    "selected.token, selected.kind, selected.created_at::text, "
                    "delivery.delivered_at::text",
                    limit);
                std::vector<RecoveryDelivery> output;
                for (const pqxx::row& row : rows) {
                    output.push_back({row[0].as<std::string>(),
                                      row[1].as<std::string>(),
                                      row[2].as<std::string>(),
                                      row[3].as<std::string>(),
                                      row[4].as<std::string>(),
                                      row[5].as<std::string>(),
                                      row[6].as<std::string>()});
                }
                return output;
            });
    }

    Result<std::vector<RecoveryNotification>>
    list_recovery_notifications(const std::string& user_id) override {
        return with_tx<std::vector<RecoveryNotification>>(
            [&](pqxx::work& tx) -> Result<std::vector<RecoveryNotification>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT id::text, recovery_id::text, user_id::text, device_id::text, kind, "
                    "created_at::text FROM offline_recovery_notifications "
                    "WHERE user_id = $1::uuid ORDER BY created_at DESC",
                    user_id);
                std::vector<RecoveryNotification> output;
                for (const pqxx::row& row : rows) {
                    output.push_back({row[0].as<std::string>(),
                                      row[1].as<std::string>(),
                                      row[2].as<std::string>(),
                                      row[3].as<std::string>(),
                                      row[4].as<std::string>(),
                                      row[5].as<std::string>()});
                }
                return output;
            });
    }

    Result<RecoveryContact> configure_recovery_contact(const RecoveryContact& input) override {
        if (input.repository_id.empty() || input.contact_taap_id.empty() ||
            input.wrapped_key.empty()) {
            return std::unexpected(StoreError{"invalid_input", "recovery contact is incomplete"});
        }
        return with_tx<RecoveryContact>([&](pqxx::work& tx) -> Result<RecoveryContact> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO offline_recovery_contacts "
                "(repository_id, contact_taap_id, wrapped_key) VALUES ($1::uuid, $2, $3) "
                "ON CONFLICT (repository_id) DO UPDATE SET contact_taap_id = "
                "EXCLUDED.contact_taap_id, "
                "wrapped_key = EXCLUDED.wrapped_key RETURNING repository_id::text, "
                "contact_taap_id, wrapped_key, created_at::text, updated_at::text",
                input.repository_id,
                input.contact_taap_id,
                input.wrapped_key);
            return RecoveryContact{rows[0][0].as<std::string>(),
                                   rows[0][1].as<std::string>(),
                                   rows[0][2].as<std::string>(),
                                   rows[0][3].as<std::string>(),
                                   rows[0][4].as<std::string>()};
        });
    }

    Result<RecoveryContact> get_recovery_contact(const std::string& repository_id) override {
        return with_tx<RecoveryContact>([&](pqxx::work& tx) -> Result<RecoveryContact> {
            const pqxx::result rows = tx.exec_params(
                "SELECT repository_id::text, contact_taap_id, wrapped_key, created_at::text, "
                "updated_at::text FROM offline_recovery_contacts WHERE repository_id = $1::uuid",
                repository_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "repository has no recovery contact"});
            }
            return RecoveryContact{rows[0][0].as<std::string>(),
                                   rows[0][1].as<std::string>(),
                                   rows[0][2].as<std::string>(),
                                   rows[0][3].as<std::string>(),
                                   rows[0][4].as<std::string>()};
        });
    }

private:
    Result<PkiCertificateRecord> get_pki_certificate_tx(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            "SELECT id::text, authority_id::text, customer_id::text, profile, subject, serial, "
            "public_value, to_char(expires_at AT TIME ZONE 'UTC', "
            "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), issued_at::text, "
            "coalesce(renewed_from_id::text,''), coalesce(revoked_at::text,'') "
            "FROM pki_certificates WHERE id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "certificate does not exist"});
        }
        return PkiCertificateRecord{rows[0][0].as<std::string>(),
                                    rows[0][1].as<std::string>(),
                                    rows[0][2].as<std::string>(),
                                    rows[0][3].as<std::string>(),
                                    rows[0][4].as<std::string>(),
                                    rows[0][5].as<std::string>(),
                                    rows[0][6].as<std::string>(),
                                    rows[0][7].as<std::string>(),
                                    rows[0][8].as<std::string>(),
                                    rows[0][9].as<std::string>(),
                                    rows[0][10].as<std::string>()};
    }

    Result<DynamicSecretLease> dynamic_secret_lease_tx(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows =
            tx.exec_params("SELECT id::text, mount_id::text, role, expires_at::text, "
                           "coalesce(revoked_at::text,''), renewals, created_at::text "
                           "FROM secrets_dynamic_leases WHERE id = $1::uuid",
                           id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "dynamic-secret lease does not exist"});
        }
        return DynamicSecretLease{rows[0][0].as<std::string>(),
                                  rows[0][1].as<std::string>(),
                                  rows[0][2].as<std::string>(),
                                  rows[0][3].as<std::string>(),
                                  rows[0][4].as<std::string>(),
                                  rows[0][5].as<int>(),
                                  rows[0][6].as<std::string>()};
    }

    Result<Application> application_by(std::string_view predicate, const std::string& value) {
        return with_tx<Application>([&](pqxx::work& tx) -> Result<Application> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + application_columns +
                                   " FROM auth_applications WHERE " + std::string(predicate),
                               value);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "application does not exist"});
            }
            return application_of(rows[0]);
        });
    }

    Result<ApplicationUser> application_user_tx(pqxx::work& tx,
                                                const std::string& application_id,
                                                const std::string& user_id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + application_user_columns +
                " FROM auth_app_users JOIN users ON users.id = auth_app_users.user_id WHERE "
                "auth_app_users.application_id = $1::uuid AND auth_app_users.user_id = $2::uuid",
            application_id,
            user_id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "application user does not exist"});
        }
        return application_user_of(rows[0]);
    }

    Result<void> require_application_user_active_tx(pqxx::work& tx,
                                                    const std::string& pool_id,
                                                    const std::string& user_id) {
        if (pool_id == "lake") {
            return {};
        }
        const pqxx::result rows = tx.exec_params(
            "SELECT auth_app_users.status FROM auth_applications JOIN auth_app_users ON "
            "auth_app_users.application_id = auth_applications.id WHERE "
            "auth_applications.pool_id = $1 AND auth_app_users.user_id = $2::uuid",
            pool_id,
            user_id);
        if (rows.empty() || rows[0][0].as<std::string>() != "active") {
            return std::unexpected(StoreError{"unauthorized", "application user is suspended"});
        }
        return {};
    }

    bool organization_admin(pqxx::work& tx,
                            const std::string& organization_id,
                            const std::string& principal_taap_id) {
        const pqxx::result rows =
            tx.exec_params("SELECT 1 FROM memberships WHERE organization_id = $1::uuid "
                           "AND principal_taap_id = $2 AND role IN ('owner', 'admin') LIMIT 1",
                           organization_id,
                           principal_taap_id);
        return !rows.empty();
    }

    void notify_devices(pqxx::work& tx, const RecoveryAttempt& attempt, const std::string& kind) {
        tx.exec_params("INSERT INTO offline_recovery_notifications "
                       "(recovery_id, user_id, device_id, kind) "
                       "SELECT $1::uuid, $2::uuid, id, $3 FROM devices WHERE user_id = $2::uuid",
                       attempt.id,
                       attempt.user_id,
                       kind);
    }

    Result<SecretMount> secret_mount_tx(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(std::string("SELECT ") + secret_mount_columns +
                                                     " FROM secrets_mounts WHERE id = $1::uuid",
                                                 id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no secret mount " + id});
        }
        return secret_mount_of(rows[0]);
    }

    Result<EncryptionKey>
    encryption_key_tx(pqxx::work& tx, const std::string& mount_id, const std::string& name) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + encryption_key_columns +
                " FROM secrets_encryption_keys WHERE mount_id = $1::uuid AND name = $2",
            mount_id,
            name);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "encryption key does not exist"});
        }
        return encryption_key_of(rows[0]);
    }

    Result<secrets_crypto::Key> customer_kek_tx(pqxx::work& tx,
                                                const std::string& customer_id,
                                                const std::string& instance_kek,
                                                bool create) {
        auto root = secrets_crypto::instance_key(instance_kek);
        if (!root) {
            return std::unexpected(root.error());
        }
        const pqxx::result rows =
            tx.exec_params("SELECT convert_from(wrapped_key, 'UTF8'), version FROM keystore "
                           "WHERE customer_id = $1::uuid FOR UPDATE",
                           customer_id);
        if (!rows.empty()) {
            const std::uint64_t version = rows[0][1].as<std::uint64_t>();
            const std::string associated =
                "lake customer kek/" + customer_id + "/" + std::to_string(version);
            auto opened = secrets_crypto::open(*root, rows[0][0].as<std::string>(), associated);
            secrets_crypto::cleanse(*root);
            if (!opened || opened->size() != secrets_crypto::Key{}.size()) {
                return std::unexpected(opened ? StoreError{"store_error", "customer key is invalid"}
                                              : opened.error());
            }
            secrets_crypto::Key output{};
            std::copy(opened->begin(), opened->end(), output.begin());
            return output;
        }
        if (!create) {
            secrets_crypto::cleanse(*root);
            return std::unexpected(StoreError{"not_found", "no customer key exists"});
        }
        const pqxx::result customer =
            tx.exec_params("SELECT id FROM customers WHERE id = $1::uuid", customer_id);
        if (customer.empty()) {
            secrets_crypto::cleanse(*root);
            return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
        }
        auto generated = secrets_crypto::random_key();
        if (!generated) {
            secrets_crypto::cleanse(*root);
            return std::unexpected(generated.error());
        }
        const std::string associated = "lake customer kek/" + customer_id + "/1";
        const std::string raw(reinterpret_cast<const char*>(generated->data()), generated->size());
        auto wrapped = secrets_crypto::seal(*root, raw, associated);
        secrets_crypto::cleanse(*root);
        if (!wrapped) {
            secrets_crypto::cleanse(*generated);
            return std::unexpected(wrapped.error());
        }
        tx.exec_params("INSERT INTO keystore (purpose, wrapped_key, version, customer_id) "
                       "VALUES ($1, convert_to($2, 'UTF8'), 1, $3::uuid)",
                       "customer-kek/" + customer_id,
                       *wrapped,
                       customer_id);
        return generated;
    }

    Result<void> store_data_key_tx(pqxx::work& tx,
                                   const std::string& customer_id,
                                   const std::string& owner_kind,
                                   const std::string& owner_id,
                                   std::uint64_t version,
                                   const secrets_crypto::Key& key,
                                   const std::string& instance_kek) {
        auto customer_key = customer_kek_tx(tx, customer_id, instance_kek, true);
        if (!customer_key) {
            return std::unexpected(customer_key.error());
        }
        const std::string associated =
            "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(version);
        const std::string raw(reinterpret_cast<const char*>(key.data()), key.size());
        auto wrapped = secrets_crypto::seal(*customer_key, raw, associated);
        secrets_crypto::cleanse(*customer_key);
        if (!wrapped) {
            return std::unexpected(wrapped.error());
        }
        tx.exec_params("INSERT INTO secrets_data_keys "
                       "(customer_id, owner_kind, owner_id, key_version, wrapped_key) "
                       "VALUES ($1::uuid, $2, $3::uuid, $4, $5)",
                       customer_id,
                       owner_kind,
                       owner_id,
                       version,
                       *wrapped);
        return {};
    }

    Result<secrets_crypto::Key> data_key_tx(pqxx::work& tx,
                                            const std::string& owner_kind,
                                            const std::string& owner_id,
                                            std::uint64_t version,
                                            const std::string& instance_kek) {
        const pqxx::result rows =
            tx.exec_params("SELECT customer_id::text, wrapped_key FROM secrets_data_keys "
                           "WHERE owner_kind = $1 AND owner_id = $2::uuid AND key_version = $3",
                           owner_kind,
                           owner_id,
                           version);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no data key version exists"});
        }
        const std::string customer_id = rows[0][0].as<std::string>();
        auto customer_key = customer_kek_tx(tx, customer_id, instance_kek, false);
        if (!customer_key) {
            return std::unexpected(customer_key.error());
        }
        const std::string associated =
            "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(version);
        auto opened = secrets_crypto::open(*customer_key, rows[0][1].as<std::string>(), associated);
        secrets_crypto::cleanse(*customer_key);
        if (!opened || opened->size() != secrets_crypto::Key{}.size()) {
            return std::unexpected(opened ? StoreError{"store_error", "data key is invalid"}
                                          : opened.error());
        }
        secrets_crypto::Key output{};
        std::copy(opened->begin(), opened->end(), output.begin());
        return output;
    }

    Result<SecretMetadata>
    secret_metadata_tx(pqxx::work& tx, const std::string& mount_id, const std::string& name) {
        const pqxx::result entries = tx.exec_params(
            "SELECT id::text, current_version, custom_metadata::text, created_at::text, "
            "updated_at::text FROM secrets_entries WHERE mount_id = $1::uuid AND name = $2",
            mount_id,
            name);
        if (entries.empty()) {
            return std::unexpected(StoreError{"not_found", "secret does not exist"});
        }
        SecretMetadata metadata{mount_id,
                                name,
                                entries[0][1].as<std::uint64_t>(),
                                nlohmann::json::parse(entries[0][2].as<std::string>()),
                                {},
                                entries[0][3].as<std::string>(),
                                entries[0][4].as<std::string>()};
        const pqxx::result versions =
            tx.exec_params("SELECT version, created_at::text, coalesce(deleted_at::text,''), "
                           "coalesce(destroyed_at::text,'') FROM secrets_versions "
                           "WHERE entry_id = $1::uuid ORDER BY version",
                           entries[0][0].as<std::string>());
        for (const pqxx::row& row : versions) {
            metadata.versions.push_back(SecretVersionMetadata{row[0].as<std::uint64_t>(),
                                                              row[1].as<std::string>(),
                                                              row[2].as<std::string>(),
                                                              row[3].as<std::string>()});
        }
        return metadata;
    }

    Result<TransitValue> encrypt_tx(pqxx::work& tx,
                                    const SecretMount& mount,
                                    const EncryptionKey& key,
                                    const std::string& plaintext,
                                    const std::string& instance_kek) {
        auto decoded = secrets_crypto::base64url_decode(plaintext);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto raw = data_key_tx(tx, "encryption", key.id, key.current_version, instance_kek);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto encryption_key = secrets_crypto::derive(*raw, "lake transit encryption v1");
        secrets_crypto::cleanse(*raw);
        if (!encryption_key) {
            return std::unexpected(encryption_key.error());
        }
        const std::string associated =
            "lake transit/" + key.id + "/" + std::to_string(key.current_version);
        auto sealed = secrets_crypto::seal(*encryption_key, *decoded, associated);
        secrets_crypto::cleanse(*encryption_key);
        if (!sealed) {
            return std::unexpected(sealed.error());
        }
        static_cast<void>(mount);
        return TransitValue{
            secrets_crypto::format_ciphertext({key.id, key.current_version, *sealed}),
            key.current_version,
            "aes-256-gcm"};
    }

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
        if (instance_kek_.empty()) {
            return std::unexpected(StoreError{
                "store_error", "LAKE_INSTANCE_KEK is required for encrypted credentials"});
        }
        const auto decoded = decode_hex(instance_kek_);
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
    std::string instance_kek_;
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> connection_;
};

} // namespace

std::unique_ptr<Store> make_postgres_store(std::string connection_uri, std::string instance_kek) {
    return std::make_unique<PostgresStore>(std::move(connection_uri), std::move(instance_kek));
}

} // namespace lake::control
