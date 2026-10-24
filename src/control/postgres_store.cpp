// The one translation unit that names libpqxx. Each call opens (or reuses) one
// connection and runs one transaction, inline on the caller's thread; SQL
// errors map to the store's error codes.
#include "lake/control/postgres_store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
#include "lake/control/own_identity.hpp"
#include "network_internal.hpp"
#include "secrets_crypto.hpp"

#include <pqxx/pqxx>

#include "lake/control/policy.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

namespace {

// The credential purposes Store::encrypt_application_credential serves.
bool application_credential_purpose(std::string_view purpose) {
    return purpose.starts_with("application.jwt.") || purpose.starts_with("application.webhook.");
}

// A task id lake made, or a parent task id it names: canonical UUID text.
bool agent_task_uuid(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? value[i] != '-' : !std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

// Audit events about a workspace's data (lake#317, migration 0063): the workspace each names in
// its details or through its object; $1 is the workspace actions. Events no live or deleted
// workspace of their space can be found for get no row and stay central.
constexpr std::string_view legacy_audit_event_assignment_sql =
    "INSERT INTO workspace_assignments (object_table, object_id, customer_id, workspace_id, "
    "outcome) SELECT 'audit_events', r.id, w.customer_id, CASE WHEN w.desired = 'deleted' THEN "
    "NULL ELSE w.id END, CASE WHEN w.desired = 'deleted' THEN 'workspace_deleted' ELSE "
    "'event_workspace' END FROM (SELECT e.id, e.customer_id, COALESCE("
    "CASE WHEN e.details->>'workspace_id' ~ '^[0-9a-f-]{36}$' THEN "
    "(e.details->>'workspace_id')::uuid END, "
    "(SELECT p.workspace_id FROM repositories p WHERE p.id = "
    "substring(e.object FROM '^repository/([0-9a-f-]{36})')::uuid), "
    "(SELECT p.workspace_id FROM repositories p WHERE p.id::text = "
    "COALESCE(e.details->>'repository_id', e.details->>'repository')), "
    "(SELECT a.workspace_id FROM apps a WHERE a.id = "
    "substring(e.object FROM '^app/([0-9a-f-]{36})')::uuid), "
    "(SELECT b.workspace_id FROM builds b WHERE b.id = "
    "substring(e.object FROM '^build/([0-9a-f-]{36})')::uuid), "
    "(SELECT m.workspace_id FROM secret_mount_routes m WHERE m.mount_id = "
    "substring(e.object FROM '^secret_mount/([0-9a-f-]{36})')::uuid), "
    "(SELECT t.workspace_id FROM agent_tasks t WHERE t.id = "
    "substring(e.object FROM '^agent_task/([0-9a-f-]{36})')::uuid)) AS workspace_id "
    "FROM audit_events e WHERE e.action = ANY ($1::text[]) AND (e.outcome <> 'denied' OR "
    "e.kind LIKE 'forge.branch_protection.%') AND NOT EXISTS (SELECT 1 FROM "
    "workspace_assignments x WHERE x.object_table = 'audit_events' AND x.object_id = e.id)) r "
    "JOIN workspaces w ON w.id = r.workspace_id AND (r.customer_id IS NULL OR w.customer_id = "
    "r.customer_id) ON CONFLICT DO NOTHING";

} // namespace

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

std::string new_recovery_code() {
    const auto random = random_secret();
    return random.size() == 64 ? "rc1_" + random.substr(0, 32) : std::string();
}

std::string recovery_selector(std::string_view code) {
    if (code.size() != 36 || !code.starts_with("rc1_") ||
        code.substr(4).find_first_not_of("0123456789abcdef") != std::string_view::npos)
        return {};
    const auto digest = sha256_hex(code);
    return digest.empty() ? std::string() : "rc1:" + digest;
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

namespace {

std::vector<std::byte> as_bytes_of(const std::vector<unsigned char>& value) {
    std::vector<std::byte> output(value.size());
    std::ranges::transform(value, output.begin(), [](auto byte) { return std::byte{byte}; });
    return output;
}

// The identifier a key derives, as liquid writes it: `taap:1:<algorithm>:<fingerprint>`, and the
// identifier its signatures name.
std::string derived_taap_id(const liquid::actor& subject, const std::string& fingerprint) {
    return "taap:1:" + subject.algorithm + ":" + fingerprint;
}

} // namespace

bool identity_document_is_self_certifying(const liquid::identity_document& document) {
    // lake#917: an initial document whose identifier is the one its own key derives. liquid's
    // verify_document checks only that the document is self-signed by the key it carries and that
    // the identifier ends with the claimed fingerprint, so a document naming someone else's
    // identifier with the caller's key verifies too. Only this check binds key and identifier.
    return !document.previous_document_hash && !document.subject.key_fingerprint.empty() &&
           document.subject.fingerprint == document.subject.key_fingerprint &&
           document.subject.taap_id ==
               derived_taap_id(document.subject, document.subject.key_fingerprint);
}

std::expected<liquid::actor, std::string>
bound_taap_key(std::string_view identity_document, std::span<const std::string> identity_chain) {
    // The documents, oldest first: the chain when one is given (it must end with the presented
    // document), else the presented document alone.
    std::vector<std::string_view> encoded_documents;
    if (identity_chain.empty()) {
        encoded_documents.push_back(identity_document);
    } else {
        if (identity_chain.size() > 64) {
            return std::unexpected("TAAP identity chain has more than 64 documents");
        }
        if (identity_chain.back() != identity_document) {
            return std::unexpected(
                "TAAP identity chain must end with the presented identity document");
        }
        for (const std::string& encoded : identity_chain) {
            encoded_documents.push_back(encoded);
        }
    }
    std::vector<liquid::identity_document> documents;
    documents.reserve(encoded_documents.size());
    for (const std::string_view encoded : encoded_documents) {
        const std::vector<unsigned char> bytes = from_base64url(encoded);
        if (bytes.empty()) {
            return std::unexpected("TAAP identity document is not base64url");
        }
        const std::vector<std::byte> canonical_input = as_bytes_of(bytes);
        auto document = liquid::decode_identity_document(canonical_input);
        if (!document) {
            return std::unexpected("TAAP identity document is invalid canonical CBOR");
        }
        const auto canonical = liquid::encode_identity_document(*document);
        if (!canonical || *canonical != canonical_input) {
            return std::unexpected("TAAP identity document does not verify canonically");
        }
        documents.push_back(std::move(*document));
    }
    const liquid::identity_document& root = documents.front();
    const auto verified_root = liquid::verify_document(root);
    if (!verified_root || !*verified_root) {
        return std::unexpected("TAAP identity document does not verify canonically");
    }
    if (!identity_document_is_self_certifying(root)) {
        return std::unexpected(
            documents.size() == 1 && root.previous_document_hash
                ? "TAAP identity document is a rotated successor: send the identity chain from "
                  "the identity's first document (identity_chain)"
                : "TAAP identity document's key is not the key its identifier was derived from");
    }
    if (documents.size() == 1) {
        return root.subject;
    }
    const auto chain = liquid::verify_chain(documents);
    if (!chain) {
        return std::unexpected("TAAP identity chain does not verify: " + chain.error().message);
    }
    // Every rotation must be signed by the key it replaces. liquid also accepts a successor that
    // declares the previous key compromised and is signed by the new key alone; a new key bound
    // that way could have been made by anyone, so it is refused here. A person who lost their key
    // returns through account recovery (docs/api/recovery.md).
    for (std::size_t index = 1; index < documents.size(); ++index) {
        const liquid::actor& previous = documents[index - 1].subject;
        const std::string previous_key = derived_taap_id(previous, previous.key_fingerprint);
        if (std::ranges::none_of(documents[index].signatures, [&](const liquid::signature& value) {
                return value.signing_key_identifier == previous_key;
            })) {
            return std::unexpected("TAAP identity rotation is not signed by the key it replaces");
        }
    }
    return chain->subject;
}

std::string claimed_taap_id(std::string_view identity_document) {
    const std::vector<unsigned char> bytes = from_base64url(identity_document);
    if (bytes.empty()) {
        return {};
    }
    const auto document = liquid::decode_identity_document(as_bytes_of(bytes));
    return document ? document->subject.taap_id : std::string{};
}

std::expected<VerifiedTaapSignIn, std::string>
verify_taap_sign_in(std::string_view identity_document,
                    std::string_view pool_id,
                    std::string_view nonce,
                    std::int64_t issued_at,
                    const nlohmann::json& signature,
                    std::int64_t now_unix,
                    std::span<const std::string> identity_chain) {
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
    const std::vector<unsigned char> signature_bytes = from_base64url(signature.value("bytes", ""));
    if (signature_bytes.empty()) {
        return std::unexpected("TAAP sign-in document or signature is not base64url");
    }
    // lake#917: the key the assertion must be signed with is the one bound to the identifier: an
    // initial document's own key, or the current key of a chain from that document whose every
    // rotation the replaced key signed.
    const auto key = bound_taap_key(identity_document, identity_chain);
    if (!key) {
        return std::unexpected(key.error());
    }
    liquid::signature signed_value{signature.value("algorithm", ""),
                                   signature.value("signing_key_identifier", ""),
                                   signature.value("signing_key_label", ""),
                                   signature.value("signature_purpose", ""),
                                   signature.value("signed_at", ""),
                                   as_bytes_of(signature_bytes)};
    // The assertion names the bound key, so liquid verifies it with that key and no other.
    if (signed_value.signing_key_identifier != derived_taap_id(*key, key->key_fingerprint)) {
        return std::unexpected("TAAP sign-in signature is not made with the identity's key");
    }
    const std::string body =
        std::string(pool_id) + "\n" + std::string(nonce) + "\n" + std::to_string(issued_at);
    const std::span<const std::byte> body_bytes(reinterpret_cast<const std::byte*>(body.data()),
                                                body.size());
    // The key was bound to the identifier above (bound_taap_key: a self-certifying initial
    // document, or a chain whose every rotation the replaced key signed), so liquid checks only
    // that this key made the signature. liquid::verify(actor, ...) would look for a stored chain
    // and refuse an identity this process does not hold (liquid#383).
    const auto verified =
        liquid::verify_key_signature(*key, "lake_sign_in", body_bytes, signed_value);
    if (!verified || !*verified) {
        return std::unexpected("TAAP sign-in signature does not verify");
    }
    return VerifiedTaapSignIn{
        key->taap_id,
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
    return OidcClaims{subject, claims.value("email", ""), claims.value("name", ""), issued_at};
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

Result<Key> derive_hkdf(const Key& key, std::string_view label) {
    EVP_KDF* algorithm = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (algorithm == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not load HKDF"});
    }
    EVP_KDF_CTX* context = EVP_KDF_CTX_new(algorithm);
    EVP_KDF_free(algorithm);
    if (context == nullptr) {
        return std::unexpected(StoreError{"store_error", "could not allocate HKDF"});
    }
    Key output{};
    char digest[] = "SHA256";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_KEY, const_cast<unsigned char*>(key.data()), key.size()),
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_INFO, const_cast<char*>(label.data()), label.size()),
        OSSL_PARAM_construct_end(),
    };
    const int result = EVP_KDF_derive(context, output.data(), output.size(), parameters);
    EVP_KDF_CTX_free(context);
    if (result != 1) {
        cleanse(output);
        return std::unexpected(StoreError{"store_error", "could not derive HKDF subkey"});
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
        return std::unexpected(StoreError{"invalid_input", "ciphertext is not a lake:v1 value"});
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
    if (value.empty() || value.size() % 4 == 1 ||
        value.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") !=
            std::string_view::npos) {
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
    "created_at::text, updated_at::text, coalesce(disabled_at::text, ''), clerk_suspended";
constexpr const char* organization_columns =
    "id::text, coalesce(taap_id,''), display_name, created_at::text, "
    "updated_at::text";
constexpr const char* customer_columns = "id::text, customer_type, coalesce(user_id::text,''), "
                                         "coalesce(organization_id::text,''), name, "
                                         "created_at::text, updated_at::text";

// The routing row only (lake#315 part 2): a repository's description is its settings record in
// its workspace, and migration 0070 dropped the retired column. The fourth column stays an empty
// placeholder so the row layout is unchanged.
constexpr const char* repository_columns =
    "id::text, customer_id::text, name, ''::text, visibility, "
    "coalesce(deleted_at::text,''), coalesce(retention_until::text,''), "
    "created_at::text, updated_at::text, coalesce(workspace_id::text,''), "
    "coalesce(liquid_id,'')";
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
                      r[8].as<std::string>(),
                      r[9].as<std::string>(),
                      r[10].as<std::string>()};
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
                r[5].as<std::string>(),
                r[6].as<std::string>(),
                r[7].as<bool>()};
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
                   nlohmann::json::parse(r[11].as<std::string>()).get<std::vector<std::string>>(),
                   r[12].as<std::string>()};
}

// ls1.<public UUID>.<256-bit random secret>; reject malformed input before opening a transaction.
std::optional<std::string> session_lookup_id(std::string_view bearer) {
    if (bearer.size() != 105 || !bearer.starts_with("ls1.") || bearer[40] != '.') {
        return std::nullopt;
    }
    for (std::size_t index = 4; index < bearer.size(); ++index) {
        if (index == 40) {
            continue;
        }
        if (index == 12 || index == 17 || index == 22 || index == 27) {
            if (bearer[index] != '-') {
                return std::nullopt;
            }
        } else if (!((bearer[index] >= '0' && bearer[index] <= '9') ||
                     (bearer[index] >= 'a' && bearer[index] <= 'f'))) {
            return std::nullopt;
        }
    }
    return std::string(bearer.substr(4, 36));
}

bool active_session_lineage(pqxx::work& tx,
                            const std::string& session_id,
                            const std::string& user_id,
                            const std::string& pool_id) {
    const auto rows = tx.exec_params(
        "WITH RECURSIVE lineage AS ("
        "SELECT id, parent_session_id, user_id, pool_id, device_id, expires_at, revoked_at "
        "FROM sessions WHERE id = $1::uuid AND user_id = $2::uuid AND pool_id = $3 "
        "UNION SELECT p.id, p.parent_session_id, p.user_id, p.pool_id, p.device_id, "
        "p.expires_at, p.revoked_at FROM sessions p JOIN lineage c ON p.id = c.parent_session_id) "
        "SELECT count(*) > 0 AND bool_or(parent_session_id IS NULL) AND "
        "bool_and(lineage.user_id = $2::uuid AND lineage.pool_id = $3 AND "
        "lineage.revoked_at IS NULL AND (expires_at IS NULL OR expires_at > clock_timestamp()) "
        "AND (devices.id IS NULL OR devices.revoked_at IS NULL)) "
        "FROM lineage LEFT JOIN devices ON devices.id = lineage.device_id",
        session_id,
        user_id,
        pool_id);
    return rows[0][0].as<bool>(false);
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
        row[12].as<std::string>(),
        row.size() > 13 ? row[13].as<std::string>() : std::string()};
}

// The agent_sessions columns agent_session_of reads. A never-expiring key is stored with
// expires_at = 'infinity' (lake#234) and read back as expires_unix 0 and expires_at "never".
constexpr const char* agent_session_columns =
    "id::text, customer_id::text, durable_agent_taap_id, dispatcher_taap_id, "
    "session_taap_id, public_key, array_to_json(scopes)::text, attestation, "
    "transparency_index, transparency_root, "
    "CASE WHEN expires_at = 'infinity' THEN 0 ELSE extract(epoch from expires_at)::bigint END, "
    "to_char(issued_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
    "CASE WHEN expires_at = 'infinity' THEN 'never' ELSE "
    "to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') END, "
    "coalesce(to_char(revoked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')";

constexpr const char* device_columns =
    "devices.id::text, devices.user_id::text, devices.name, "
    "coalesce(devices.revoked_at::text,''), devices.created_at::text, "
    "devices.updated_at::text, coalesce(passkeys.subject,''), "
    "coalesce(passkeys.public_data->'binding_attestation','null'::jsonb)::text";
constexpr const char* session_columns =
    "id::text, coalesce(user_id::text,''), coalesce(device_id::text,''), "
    "pool_id, kind, name, reason, coalesce(expires_at::text,''), "
    "coalesce(revoked_at::text,''), created_at::text, "
    "coalesce(last_used_at::text,''), array_to_json(scopes)::text, "
    "coalesce(parent_session_id::text,'')";
constexpr const char* qualified_session_columns =
    "sessions.id::text, coalesce(sessions.user_id::text,''), "
    "coalesce(sessions.device_id::text,''), sessions.pool_id, sessions.kind, "
    "sessions.name, sessions.reason, coalesce(sessions.expires_at::text,''), "
    "coalesce(sessions.revoked_at::text,''), sessions.created_at::text, "
    "coalesce(sessions.last_used_at::text,''), "
    "array_to_json(sessions.scopes)::text, coalesce(sessions.parent_session_id::text,'')";
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
// The routing columns of auth_applications (lake#351). An application's settings (name, allowed
// origins, redirect URIs, theme) are its record in its workspace; migration 0070 dropped the
// retired columns.
constexpr const char* application_columns =
    "id::text, customer_id::text, pool_id, coalesce(custom_sign_in_domain,''), created_at::text, "
    "updated_at::text, coalesce(workspace_id::text,'')";

Application application_of(const pqxx::row& row) {
    Application application;
    application.id = row[0].as<std::string>();
    application.customer_id = row[1].as<std::string>();
    application.pool_id = row[2].as<std::string>();
    application.custom_sign_in_domain = row[3].as<std::string>();
    application.created_at = row[4].as<std::string>();
    application.updated_at = row[5].as<std::string>();
    application.workspace_id = row[6].as<std::string>();
    return application;
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

class PostgresStore final : public Store {
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

    Result<User> create_user(const NewUser& in) override {
        if (auto checked = check_principal_id(in.taap_id, "taap_id"); !checked) {
            return std::unexpected(checked.error());
        }
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
                                   " FROM users WHERE deleted_at IS NULL ORDER BY created_at DESC "
                                   "LIMIT $1",
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
                std::string("SELECT ") + user_columns +
                    " FROM users WHERE id = $1::uuid AND deleted_at IS NULL",
                id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no user " + id});
            return user_of(rows[0]);
        });
    }

    Result<std::map<std::string, std::string>>
    principal_names(const std::vector<std::string>& taap_ids) override {
        if (taap_ids.empty()) {
            return std::map<std::string, std::string>{};
        }
        return with_tx<std::map<std::string, std::string>>(
            [&](pqxx::work& tx) -> Result<std::map<std::string, std::string>> {
                const pqxx::result rows = tx.exec_params(
                    "WITH wanted AS (SELECT jsonb_array_elements_text($1::jsonb) AS taap_id) "
                    "SELECT users.taap_id, COALESCE(NULLIF(users.display_name, ''), users.email) "
                    "FROM users JOIN wanted USING (taap_id) WHERE users.deleted_at IS NULL "
                    "UNION ALL SELECT organizations.taap_id, organizations.display_name "
                    "FROM organizations JOIN wanted USING (taap_id) "
                    "WHERE organizations.display_name <> ''",
                    nlohmann::json(taap_ids).dump());
                std::map<std::string, std::string> names;
                for (const pqxx::row& row : rows) {
                    if (!row[1].is_null()) {
                        names[row[0].as<std::string>()] = row[1].as<std::string>();
                    }
                }
                return names;
            });
    }

    Result<Organization> create_organization(const NewOrganization& in) override {
        if (auto checked = check_principal_id(in.taap_id, "taap_id"); !checked) {
            return std::unexpected(checked.error());
        }
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
                                       " FROM organizations WHERE deleted_at IS NULL "
                                       "ORDER BY created_at DESC LIMIT $1",
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
                                                         " FROM organizations WHERE id = $1::uuid "
                                                         "AND deleted_at IS NULL",
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
    Result<std::vector<Customer>> list_customers(int limit, const std::string& user_id) override {
        return with_tx<std::vector<Customer>>([&](pqxx::work& tx) -> Result<std::vector<Customer>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + customer_columns +
                                   " FROM customers WHERE ($2 = '' OR user_id::text = $2) "
                                   "AND deleted_at IS NULL "
                                   "ORDER BY created_at DESC LIMIT $1",
                               limit,
                               user_id);
            std::vector<Customer> out;
            for (const auto& r : rows)
                out.push_back(customer_of(r));
            return out;
        });
    }
    Result<Customer> get_customer(const std::string& id) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + customer_columns +
                                                         " FROM customers WHERE id = $1::uuid "
                                                         "AND deleted_at IS NULL",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no customer " + id});
            return customer_of(rows[0]);
        });
    }

    Result<Customer> organization_customer(const std::string& organization_id) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + customer_columns +
                                   " FROM customers WHERE organization_id = $1::uuid "
                                   "AND deleted_at IS NULL",
                               organization_id);
            if (rows.empty())
                return std::unexpected(
                    StoreError{"not_found", "no account for organization " + organization_id});
            return customer_of(rows[0]);
        });
    }

    Result<Repository> create_repository(const NewRepository& in) override {
        if (!in.description.empty()) {
            return std::unexpected(StoreError{
                "invalid_input",
                "a repository's description is its settings record in its workspace (lake#315)"});
        }
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                               "'customer' AND external_id = $1",
                               in.customer_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "customer_id does not reference a policy object"});
            }
            // Migration 0051's routing trigger refuses the same; this names the field.
            const pqxx::result workspace = tx.exec_params(
                "SELECT 1 FROM workspaces WHERE id::text = $1 AND customer_id = $2::uuid AND "
                "desired <> 'deleted'",
                in.workspace_id,
                in.customer_id);
            if (workspace.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "workspace_id must name a workspace in this account"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO repositories (id, customer_id, name, "
                                           "visibility, workspace_id) VALUES "
                                           "(COALESCE(NULLIF($1, '')::uuid, gen_random_uuid()), "
                                           "$2::uuid, $3, $4, $5::uuid) RETURNING ") +
                                   repository_columns,
                               in.id,
                               in.customer_id,
                               in.name,
                               in.visibility.empty() ? std::string("private") : in.visibility,
                               in.workspace_id);
            Repository repository = repository_of(rows[0]);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('repository', $1, $2::uuid)",
                           repository.id,
                           parents[0][0].as<std::string>());
            return repository;
        });
    }
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                      int limit,
                                                      const std::string& workspace_id) override {
        return with_tx<std::vector<Repository>>(
            [&](pqxx::work& tx) -> Result<std::vector<Repository>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + repository_columns +
                        " FROM repositories WHERE ($1 = '' OR customer_id = NULLIF($1, '')::uuid) AND "
                        "($2 = '' OR workspace_id = NULLIF($2, '')::uuid) "
                        "ORDER BY created_at DESC LIMIT $3",
                    customer_id,
                    workspace_id,
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

    Result<std::vector<Repository>> repositories_by_liquid_id(const std::string& liquid_id) override {
        return with_tx<std::vector<Repository>>(
            [&](pqxx::work& tx) -> Result<std::vector<Repository>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + repository_columns +
                        " FROM repositories WHERE liquid_id = $1 AND deleted_at IS NULL "
                        "ORDER BY created_at",
                    liquid_id);
                std::vector<Repository> out;
                for (const auto& r : rows)
                    out.push_back(repository_of(r));
                return out;
            });
    }

    Result<void> set_repository_liquid_id(const std::string& id,
                                          const std::string& liquid_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            // Written only when it changed, so the updated_at trigger moves only then.
            const pqxx::result rows = tx.exec_params(
                "UPDATE repositories SET liquid_id = NULLIF($2, '') WHERE id = $1::uuid AND "
                "liquid_id IS DISTINCT FROM NULLIF($2, '') RETURNING 1",
                id,
                liquid_id);
            if (rows.empty() &&
                tx.exec_params("SELECT 1 FROM repositories WHERE id = $1::uuid", id).empty())
                return std::unexpected(StoreError{"not_found", "no repository " + id});
            return {};
        });
    }

    Result<std::vector<Repository>> repositories_without_liquid_id(int limit) override {
        return with_tx<std::vector<Repository>>(
            [&](pqxx::work& tx) -> Result<std::vector<Repository>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + repository_columns +
                        " FROM repositories WHERE liquid_id IS NULL AND deleted_at IS NULL AND "
                        "workspace_id IS NOT NULL ORDER BY created_at LIMIT $1",
                    limit);
                std::vector<Repository> out;
                for (const auto& r : rows)
                    out.push_back(repository_of(r));
                return out;
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

    Result<void>
    assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            std::string array = "{";
            for (std::size_t index = 0; index < workspace_actions.size(); ++index) {
                array += (index == 0 ? "\"" : ",\"") + workspace_actions[index] + "\"";
            }
            array += "}";
            tx.exec_params(std::string(legacy_audit_event_assignment_sql), array);
            return {};
        });
    }

    Result<std::vector<LegacyAuditEvent>> legacy_audit_events(const std::string& after_id,
                                                              int limit) override {
        return with_tx<std::vector<LegacyAuditEvent>>(
            [&](pqxx::work& tx) -> Result<std::vector<LegacyAuditEvent>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + audit_event_columns +
                        ", assigned_customer, assigned_workspace, assigned_outcome FROM "
                        "(SELECT e.*, a.customer_id::text AS assigned_customer, "
                        "COALESCE(a.workspace_id::text, '') AS assigned_workspace, "
                        "a.outcome AS assigned_outcome FROM audit_events e JOIN "
                        "workspace_assignments a ON a.object_table = 'audit_events' AND "
                        "a.object_id = e.id WHERE ($1 = '' OR e.sequence > (SELECT sequence FROM "
                        "audit_events WHERE id::text = $1))) s ORDER BY sequence LIMIT $2",
                    after_id,
                    std::clamp(limit, 1, 5000));
                std::vector<LegacyAuditEvent> events;
                for (const pqxx::row& row : rows) {
                    LegacyAuditEvent legacy;
                    legacy.event = audit_event_of(row);
                    legacy.customer_id = row[16].as<std::string>();
                    legacy.workspace_id = row[17].as<std::string>();
                    legacy.outcome = row[18].as<std::string>();
                    events.push_back(std::move(legacy));
                }
                return events;
            });
    }

    Result<DriveShareLinkRoute> drive_share_link_route(const std::string& token_hash) override {
        return with_tx<DriveShareLinkRoute>([&](pqxx::work& tx) -> Result<DriveShareLinkRoute> {
            const pqxx::result rows =
                tx.exec_params("SELECT link_id::text, workspace_id::text FROM "
                               "drive_share_link_routes WHERE token_hash = $1",
                               token_hash);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "share link is not recognized"});
            }
            return DriveShareLinkRoute{rows[0][0].as<std::string>(), rows[0][1].as<std::string>()};
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

    // An organization's own space (the customer whose organization_id is that organization)
    // belongs to the organization's people: an organization-scope membership derives the same
    // policy grant on the space's customer object as on the organization object, so members
    // reach its repositories, workspaces and apps (docs/api/organizations.md).
    static void derive_space_grant(pqxx::work& tx,
                                   const Membership& membership,
                                   const std::string& role) {
        if (!membership.team_id.empty()) {
            return;
        }
        tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, attestation_digest, "
                       "granted_by_taap_id) SELECT $1, o.id, $3, $4, $5 FROM customers c "
                       "JOIN objects o ON o.object_type = 'customer' AND o.external_id = c.id::text "
                       "WHERE c.organization_id = $2::uuid ON CONFLICT DO NOTHING",
                       membership.principal_taap_id,
                       membership.organization_id,
                       role == "member" ? "read" : "admin",
                       membership.attestation_digest,
                       membership.granted_by_taap_id);
    }

    static void drop_space_grant(pqxx::work& tx,
                                 const Membership& membership,
                                 const std::string& role) {
        if (!membership.team_id.empty()) {
            return;
        }
        tx.exec_params("DELETE FROM grants g USING customers c, objects o "
                       "WHERE c.organization_id = $2::uuid AND o.object_type = 'customer' "
                       "AND o.external_id = c.id::text AND g.object_id = o.id "
                       "AND g.principal_taap_id = $1 AND g.role = $3 "
                       "AND g.attestation_digest = $4 AND g.granted_by_taap_id = $5",
                       membership.principal_taap_id,
                       membership.organization_id,
                       role == "member" ? "read" : "admin",
                       membership.attestation_digest,
                       membership.granted_by_taap_id);
    }

    // Serialize membership decisions within an organization before reading authority or owner
    // counts. The row lock also coordinates with organization deletion.
    static void lock_membership_organization(pqxx::work& tx, const std::string& organization_id) {
        tx.exec_params("SELECT id FROM organizations WHERE id = $1::uuid FOR UPDATE",
                       organization_id);
    }

    static Result<void> require_organization_owner(pqxx::work& tx,
                                                   const std::string& organization_id,
                                                   const std::string& actor) {
        if (actor == "admin") {
            return {};
        }
        const auto owners = tx.exec_params(
            "SELECT id FROM memberships WHERE organization_id = $1::uuid AND team_id IS NULL "
            "AND role = 'owner' AND principal_taap_id = $2 FOR SHARE",
            organization_id,
            actor);
        if (!owners.empty()) {
            return {};
        }
        return std::unexpected(StoreError{
            "unauthorized", "owner changes require an organization owner or the operator"});
    }

    Result<Membership> create_membership(const NewMembership& in) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            lock_membership_organization(tx, in.organization_id);
            if (in.role == "owner") {
                if (auto allowed =
                        require_organization_owner(tx, in.organization_id, in.granted_by_taap_id);
                    !allowed) {
                    return std::unexpected(allowed.error());
                }
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
            derive_space_grant(tx, membership, in.role);
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
            const auto scope = tx.exec_params(
                "SELECT organization_id::text FROM memberships WHERE id = $1::uuid", id);
            if (scope.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            lock_membership_organization(tx, scope[0][0].as<std::string>());
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + membership_columns +
                                   " FROM memberships WHERE id = $1::uuid FOR UPDATE",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            const Membership membership = membership_of(rows[0]);
            if (membership.role == "owner" || role == "owner") {
                if (auto allowed =
                        require_organization_owner(tx, membership.organization_id, actor);
                    !allowed) {
                    return std::unexpected(allowed.error());
                }
            }
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
            drop_space_grant(tx, membership, membership.role);
            derive_space_grant(tx, membership, role);
            const pqxx::result updated = tx.exec_params(
                std::string("UPDATE memberships SET role = $2 WHERE id = $1::uuid RETURNING ") +
                    membership_columns,
                id,
                role);
            return membership_of(updated[0]);
        });
    }

    Result<void> remove_member(const std::string& id, const std::string& actor) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const auto scope = tx.exec_params(
                "SELECT organization_id::text FROM memberships WHERE id = $1::uuid", id);
            if (scope.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            lock_membership_organization(tx, scope[0][0].as<std::string>());
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + membership_columns +
                                   " FROM memberships WHERE id = $1::uuid FOR UPDATE",
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no membership " + id});
            }
            const Membership membership = membership_of(rows[0]);
            if (membership.role == "owner") {
                if (auto allowed =
                        require_organization_owner(tx, membership.organization_id, actor);
                    !allowed) {
                    return std::unexpected(allowed.error());
                }
            }
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
            drop_space_grant(tx, membership, membership.role);
            tx.exec_params("DELETE FROM objects WHERE object_type = 'member' AND external_id = $1",
                           id);
            tx.exec_params("DELETE FROM memberships WHERE id = $1::uuid", id);
            return {};
        });
    }

    Result<Invitation> create_invitation(const NewInvitation& in) override {
        return with_tx<Invitation>([&](pqxx::work& tx) -> Result<Invitation> {
            lock_membership_organization(tx, in.organization_id);
            if (in.role == "owner") {
                if (auto allowed =
                        require_organization_owner(tx, in.organization_id, in.created_by_taap_id);
                    !allowed) {
                    return std::unexpected(allowed.error());
                }
            }
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

    // Inserts the owner membership, its member object, and its organization admin grant.
    static Membership insert_owner_membership(pqxx::work& tx,
                                              const std::string& organization_id,
                                              const std::string& organization_object_id,
                                              const std::string& user_id,
                                              const std::string& taap_id,
                                              const std::string& evidence) {
        const pqxx::result rows = tx.exec_params(
            std::string("INSERT INTO memberships (organization_id, principal_type, principal_id, "
                        "principal_taap_id, role, attestation_digest, granted_by_taap_id) "
                        "VALUES ($1::uuid, 'user', $2, $3, 'owner', $4, $3) RETURNING ") +
                membership_columns,
            organization_id,
            user_id,
            taap_id,
            evidence);
        Membership membership = membership_of(rows[0]);
        tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                       "VALUES ('member', $1, $2::uuid)",
                       membership.id,
                       organization_object_id);
        tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, attestation_digest, "
                       "granted_by_taap_id) VALUES ($1, $2::uuid, 'admin', $3, $1)",
                       taap_id,
                       organization_object_id,
                       evidence);
        derive_space_grant(tx, membership, "owner");
        return membership;
    }

    static bool user_matches_identity(pqxx::work& tx,
                                      const std::string& user_id,
                                      const std::string& taap_id) {
        return !tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
                               user_id,
                               taap_id)
                    .empty();
    }

    Result<OrganizationSpace> create_organization_space(const NewOrganizationSpace& in) override {
        return with_tx<OrganizationSpace>([&](pqxx::work& tx) -> Result<OrganizationSpace> {
            if (in.owner_user_id.empty() || in.owner_taap_id.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "an organization is started by a signed-in person"});
            }
            if (!user_matches_identity(tx, in.owner_user_id, in.owner_taap_id)) {
                return std::unexpected(
                    StoreError{"invalid_input", "user principal does not match its TAAP identity"});
            }
            if (!in.name.empty() && !valid_handle(in.name)) {
                return std::unexpected(StoreError{"invalid_input",
                                                  "handle must be lowercase letters, digits and "
                                                  "inner hyphens (customer_name_format)"});
            }
            if (!in.name.empty() &&
                !tx.exec_params("SELECT 1 FROM customers WHERE name = $1", in.name).empty()) {
                return std::unexpected(
                    StoreError{"conflict", "name already taken (customer_name_unique)"});
            }
            const std::string evidence = "create_organization_space/" + in.request_id;
            OrganizationSpace space;
            space.organization = organization_of(
                tx.exec_params(std::string("INSERT INTO organizations (display_name) VALUES ($1) "
                                           "RETURNING ") +
                                   organization_columns,
                               in.display_name)[0]);
            for (int attempt = 0; attempt < 32 && space.customer.id.empty(); ++attempt) {
                const std::string name = in.name.empty() ? generate_handle() : in.name;
                const pqxx::result rows = tx.exec_params(
                    std::string("INSERT INTO customers (customer_type, organization_id, name) "
                                "VALUES ('organization', $1::uuid, $2) "
                                "ON CONFLICT (name) DO NOTHING RETURNING ") +
                        customer_columns,
                    space.organization.id,
                    name);
                if (!rows.empty()) {
                    space.customer = customer_of(rows[0]);
                } else if (!in.name.empty()) {
                    return std::unexpected(
                        StoreError{"conflict", "name already taken (customer_name_unique)"});
                }
            }
            if (space.customer.id.empty()) {
                return std::unexpected(StoreError{
                    "conflict", "could not allocate an organization handle; pass a name"});
            }
            const std::string customer_object =
                tx.exec_params("INSERT INTO objects (object_type, external_id) "
                               "VALUES ('customer', $1) RETURNING id::text",
                               space.customer.id)[0][0]
                    .as<std::string>();
            const std::string organization_object =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('organization', $1, $2::uuid) RETURNING id::text",
                               space.organization.id,
                               customer_object)[0][0]
                    .as<std::string>();
            space.owner = insert_owner_membership(tx,
                                                  space.organization.id,
                                                  organization_object,
                                                  in.owner_user_id,
                                                  in.owner_taap_id,
                                                  evidence);
            return space;
        });
    }

    Result<OrganizationSpace> convert_personal_space(const ConvertPersonalSpace& in) override {
        return with_tx<OrganizationSpace>([&](pqxx::work& tx) -> Result<OrganizationSpace> {
            if (in.owner_user_id.empty() || in.owner_taap_id.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "a person's own account is converted by the person who owns it"});
            }
            if (!user_matches_identity(tx, in.owner_user_id, in.owner_taap_id)) {
                return std::unexpected(
                    StoreError{"invalid_input", "user principal does not match its TAAP identity"});
            }
            const pqxx::result customers =
                tx.exec_params(std::string("SELECT ") + customer_columns +
                                   " FROM customers WHERE id = $1::uuid FOR UPDATE",
                               in.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
            }
            const Customer current = customer_of(customers[0]);
            if (current.customer_type != "individual") {
                return std::unexpected(
                    StoreError{"conflict", "this account already belongs to an organization"});
            }
            if (current.user_id != in.owner_user_id) {
                return std::unexpected(StoreError{
                    "unauthorized", "only the person whose own account this is can convert it"});
            }
            const pqxx::result customer_objects =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = 'customer' "
                               "AND external_id = $1",
                               in.customer_id);
            if (customer_objects.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "the account's policy object is missing"});
            }
            const std::string evidence = "convert_personal_space/" + in.request_id;
            OrganizationSpace space;
            space.organization = organization_of(
                tx.exec_params(std::string("INSERT INTO organizations (display_name) VALUES ($1) "
                                           "RETURNING ") +
                                   organization_columns,
                               in.display_name)[0]);
            space.customer = customer_of(tx.exec_params(
                std::string("UPDATE customers SET customer_type = 'organization', user_id = NULL, "
                            "organization_id = $2::uuid WHERE id = $1::uuid RETURNING ") +
                    customer_columns,
                in.customer_id,
                space.organization.id)[0]);
            const std::string organization_object =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('organization', $1, $2::uuid) RETURNING id::text",
                               space.organization.id,
                               customer_objects[0][0].as<std::string>())[0][0]
                    .as<std::string>();
            space.owner = insert_owner_membership(tx,
                                                  space.organization.id,
                                                  organization_object,
                                                  in.owner_user_id,
                                                  in.owner_taap_id,
                                                  evidence);
            const auto personal =
                provision_personal_customer(tx, in.owner_user_id, in.owner_taap_id, evidence);
            if (!personal) {
                return std::unexpected(personal.error());
            }
            space.personal = customer_of(tx.exec_params(std::string("SELECT ") + customer_columns +
                                                            " FROM customers WHERE id = $1::uuid",
                                                        *personal)[0]);
            return space;
        });
    }

    Result<std::vector<Membership>> list_user_memberships(const std::string& user_id,
                                                          int limit) override {
        return with_tx<std::vector<Membership>>(
            [&](pqxx::work& tx) -> Result<std::vector<Membership>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + membership_columns +
                        " FROM memberships WHERE principal_type = 'user' AND principal_id = $1 "
                        "ORDER BY created_at DESC LIMIT $2",
                    user_id,
                    limit);
                std::vector<Membership> out;
                for (const auto& row : rows) {
                    out.push_back(membership_of(row));
                }
                return out;
            });
    }

    Result<std::vector<Invitation>> list_invitee_invitations(const std::string& email,
                                                             const std::vector<std::string>& handles,
                                                             int limit) override {
        return with_tx<std::vector<Invitation>>(
            [&](pqxx::work& tx) -> Result<std::vector<Invitation>> {
                pqxx::params params;
                params.append(email);
                std::string handle_list;
                for (const std::string& handle : handles) {
                    std::string value = handle;
                    std::ranges::transform(value, value.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    params.append(value);
                    handle_list += (handle_list.empty() ? "$" : ", $") + std::to_string(params.size());
                }
                params.append(limit);
                const std::string handle_match =
                    handle_list.empty() ? "false" : "invitee IN (" + handle_list + ")";
                const pqxx::result rows = tx.exec(
                    std::string("SELECT ") + invitation_columns +
                        " FROM invitations WHERE status = 'pending' AND expires_at > now() "
                        "AND ((invitee_type = 'email' AND $1 <> '' AND invitee = lower($1)) OR "
                        "(invitee_type = 'handle' AND " + handle_match + ")) "
                        "ORDER BY created_at DESC LIMIT $" + std::to_string(params.size()),
                    params);
                std::vector<Invitation> out;
                for (const auto& row : rows) {
                    out.push_back(invitation_of(row));
                }
                return out;
            });
    }

    Result<Membership> accept_invitation(const AcceptInvitation& in) override {
        return with_tx<Membership>([&](pqxx::work& tx) -> Result<Membership> {
            const auto scope =
                tx.exec_params("SELECT organization_id::text FROM invitations WHERE id = $1::uuid",
                               in.invitation_id);
            if (scope.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no invitation " + in.invitation_id});
            }
            lock_membership_organization(tx, scope[0][0].as<std::string>());
            const pqxx::result invitations =
                tx.exec_params(std::string("SELECT ") + invitation_columns +
                                   " FROM invitations WHERE id = $1::uuid FOR UPDATE",
                               in.invitation_id);
            if (invitations.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no invitation " + in.invitation_id});
            }
            Invitation invitation = invitation_of(invitations[0]);
            if (invitation.role == "owner") {
                if (auto allowed = require_organization_owner(
                        tx, invitation.organization_id, in.accepted_by_taap_id);
                    !allowed) {
                    return std::unexpected(allowed.error());
                }
            }
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
            derive_space_grant(tx, membership, invitation.role);
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

    Result<PolicyObject> set_policy_object_parent(const std::string& id,
                                                  const std::string& parent_id) override {
        return with_tx<PolicyObject>([&](pqxx::work& tx) -> Result<PolicyObject> {
            // One parent change at a time, so two concurrent moves cannot close a cycle that
            // policy_paths would then walk forever.
            tx.exec("SELECT pg_advisory_xact_lock(hashtext('lake_policy_object_parents'))");
            const pqxx::result objects = tx.exec_params(
                "SELECT object_type FROM objects WHERE id = $1::uuid FOR UPDATE", id);
            if (objects.empty()) {
                return std::unexpected(StoreError{"not_found", "no policy object " + id});
            }
            if (objects[0][0].as<std::string>() != "path") {
                return std::unexpected(
                    StoreError{"invalid_input", "only a path policy object changes its parent"});
            }
            const pqxx::result parents =
                tx.exec_params("SELECT object_type FROM objects WHERE id = $1::uuid", parent_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "parent_id does not reference a policy object"});
            }
            if (!object_parent_allowed("path", parents[0][0].as<std::string>())) {
                return std::unexpected(
                    StoreError{"invalid_input", "object type is not valid under this parent"});
            }
            const pqxx::result cycle =
                tx.exec_params("WITH RECURSIVE up(id, parent_id) AS ("
                               "SELECT id, parent_id FROM objects WHERE id = $2::uuid "
                               "UNION SELECT objects.id, objects.parent_id FROM objects "
                               "JOIN up ON objects.id = up.parent_id) "
                               "SELECT 1 FROM up WHERE id = $1::uuid",
                               id,
                               parent_id);
            if (!cycle.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "a policy object cannot be put under itself"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE objects SET parent_id = $2::uuid WHERE id = $1::uuid "
                            "RETURNING ") +
                    object_columns,
                id,
                parent_id);
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
            // The routing row only (migration 0059): the settings columns stay NULL, since the
            // settings are written to the application's workspace.
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO auth_applications (id, customer_id, pool_id, "
                            "custom_sign_in_domain, workspace_id) VALUES ($1::uuid, $2::uuid, $3, "
                            "NULLIF($4, ''), NULLIF($5, '')::uuid) RETURNING ") +
                    application_columns,
                input.id,
                input.customer_id,
                input.pool_id,
                input.custom_sign_in_domain,
                input.workspace_id);
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

    Result<std::string> encrypt_application_credential(const std::string& purpose,
                                                       const std::string& plaintext) override {
        if (!application_credential_purpose(purpose) || plaintext.empty()) {
            return std::unexpected(StoreError{"invalid_input", "not an application credential"});
        }
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            return encrypt_secret(tx, purpose, plaintext);
        });
    }

    Result<std::string> decrypt_application_credential(const std::string& purpose,
                                                       const std::string& envelope) override {
        if (!application_credential_purpose(purpose)) {
            return std::unexpected(StoreError{"invalid_input", "not an application credential"});
        }
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            return decrypt_secret(tx, purpose, envelope);
        });
    }

    Result<int> revoke_application_user_sessions(const std::string& application_id,
                                                 const std::string& user_id) override {
        return with_tx<int>([&](pqxx::work& tx) -> Result<int> {
            const pqxx::result applications = tx.exec_params(
                "SELECT pool_id FROM auth_applications WHERE id = $1::uuid", application_id);
            if (applications.empty()) {
                return std::unexpected(StoreError{"not_found", "application does not exist"});
            }
            const pqxx::result revoked = tx.exec_params(
                "UPDATE sessions SET revoked_at = now() WHERE user_id = $1::uuid AND pool_id = $2 "
                "AND revoked_at IS NULL RETURNING id",
                user_id,
                applications[0][0].as<std::string>());
            return static_cast<int>(revoked.size());
        });
    }

    Result<void> delete_application_user_credentials(const std::string& application_id,
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
            return {};
        });
    }

    audit::Result<audit::Identity> current_lake_identity() override {
        return with_audit_tx<audit::Identity>([](pqxx::work& tx) -> audit::Result<audit::Identity> {
            const pqxx::result rows =
                tx.exec(std::string("SELECT ") + identity_columns +
                        " FROM lake_identity WHERE current ORDER BY created_at DESC LIMIT 1");
            if (rows.empty()) {
                return std::unexpected(audit::Error{"not_found", "no Lake identity exists"});
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
                        audit::Error{"not_found", "no Lake identity for " + taap_id});
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
                    audit::Error{"conflict", "the Lake identity changed before rotation"});
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
        return with_tx<void>([&](pqxx::work& tx) { return append_audit_tx(tx, event); });
    }

    static Result<void> append_audit_tx(pqxx::work& tx, const audit::Event& event) {
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
                        "AND ($7 = '' OR request_id = $7) "
                        "AND ($8 = '' OR sequence > "
                        "(SELECT sequence FROM audit_events WHERE id::text = $8)) "
                        "AND NOT (outcome = 'success' AND action IN "
                        "(SELECT jsonb_array_elements_text($9::jsonb))) "
                        "AND ($10 = '' OR customer_id::text = $10) "
                        "ORDER BY sequence " +
                        (filter.newest_first ? "DESC" : "ASC") + " LIMIT $6",
                    filter.principal,
                    filter.object,
                    filter.outcome,
                    filter.from,
                    filter.to,
                    filter.limit,
                    filter.request_id,
                    filter.after_id,
                    nlohmann::json(filter.omit_successful_actions).dump(),
                    filter.customer_id);
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

    static constexpr const char* service_columns =
        "s.id::text, s.customer_id::text, s.kind, s.name, s.slug, s.is_default, "
        "COALESCE((s.spec->>'instances')::int, 1), COALESCE((s.spec->>'storage_gi')::int, 10), "
        "s.desired, s.generation, s.created_by_taap_id, "
        "to_char(s.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(s.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(st.phase, ''), COALESCE(st.message, ''), COALESCE(st.endpoint, ''), "
        "COALESCE(st.observed_generation, 0), COALESCE(s.workspace_id::text, ''), "
        "COALESCE(s.spec->>'bucket', ''), COALESCE(s.spec->>'endpoint', ''), "
        "COALESCE(s.spec->>'prefix', ''), COALESCE(s.spec->'credential'->>'mount_id', ''), "
        "COALESCE(s.spec->'credential'->>'name', ''), COALESCE(s.spec->>'repository_id', '')";

    static ManagedService service_from(const pqxx::row& row) {
        ManagedService service;
        service.id = row[0].as<std::string>();
        service.customer_id = row[1].as<std::string>();
        service.kind = row[2].as<std::string>();
        service.name = row[3].as<std::string>();
        service.slug = row[4].as<std::string>();
        service.is_default = row[5].as<bool>();
        service.instances = row[6].as<int>();
        service.storage_gi = row[7].as<int>();
        service.desired = row[8].as<std::string>();
        service.generation = row[9].as<std::int64_t>();
        service.created_by_taap_id = row[10].as<std::string>();
        service.created_at = row[11].as<std::string>();
        service.updated_at = row[12].as<std::string>();
        service.phase = row[13].as<std::string>();
        service.message = row[14].as<std::string>();
        service.endpoint = row[15].as<std::string>();
        service.observed_generation = row[16].as<std::int64_t>();
        service.workspace_id = row[17].as<std::string>();
        return service;
    }

    static Result<ManagedService> one_service(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + service_columns +
                " FROM services s LEFT JOIN service_status st ON st.service_id = s.id "
                "WHERE s.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no service " + id});
        }
        return service_from(rows[0]);
    }

    static constexpr const char* workspace_columns =
        "id::text, customer_id::text, name, environment, desired, generation, COALESCE(phase,''), "
        "COALESCE(observed_generation,0), COALESCE(message,''), COALESCE(namespace,''), "
        "COALESCE(zetta_service_id::text,''), created_at::text, updated_at::text, storage_gi, "
        "storage_gi_observed, storage_phase, storage_message, COALESCE(store_moved_at::text,'')";

    static Workspace workspace_from(const pqxx::row& row) {
        Workspace value;
        value.id = row[0].as<std::string>();
        value.customer_id = row[1].as<std::string>();
        value.name = row[2].as<std::string>();
        value.environment = row[3].as<std::string>();
        value.desired = row[4].as<std::string>();
        value.generation = row[5].as<std::int64_t>();
        value.phase = row[6].as<std::string>();
        value.observed_generation = row[7].as<std::int64_t>();
        value.message = row[8].as<std::string>();
        value.namespace_name = row[9].as<std::string>();
        value.zetta_service_id = row[10].as<std::string>();
        value.created_at = row[11].as<std::string>();
        value.updated_at = row[12].as<std::string>();
        value.storage_gi = row[13].as<std::int32_t>();
        value.storage_gi_observed = row[14].as<std::int32_t>();
        value.storage_phase = row[15].as<std::string>();
        value.storage_message = row[16].as<std::string>();
        value.store_moved_at = row[17].as<std::string>();
        return value;
    }

    static Result<Workspace> one_workspace(pqxx::work& tx, const std::string& id) {
        const auto rows = tx.exec_params(std::string("SELECT ") + workspace_columns +
                                        " FROM workspaces WHERE id = $1::uuid", id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
        }
        return workspace_from(rows[0]);
    }

    Result<std::vector<Workspace>> list_workspaces(const std::string& customer_id) override {
        return with_tx<std::vector<Workspace>>([&](pqxx::work& tx) -> Result<std::vector<Workspace>> {
            const auto rows = tx.exec_params(std::string("SELECT ") + workspace_columns +
                " FROM workspaces WHERE customer_id = $1::uuid AND NOT "
                "(desired = 'deleted' AND COALESCE(phase,'') = 'Deleted') ORDER BY created_at, id", customer_id);
            std::vector<Workspace> values;
            for (const auto& row : rows) {
                values.push_back(workspace_from(row));
            }
            return values;
        });
    }

    Result<Workspace> get_workspace(const std::string& id) override {
        if (managed_service_slug("ws", id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must be a UUID"});
        }
        return with_tx<Workspace>([&](pqxx::work& tx) -> Result<Workspace> {
            return one_workspace(tx, id);
        });
    }

    Result<Workspace> create_workspace(const NewWorkspace& input) override {
        if (input.name.empty() || input.name.size() > 128 || !valid_workspace_environment(input.environment)) {
            return std::unexpected(StoreError{"invalid_input", "name and DNS-label safe environment are required"});
        }
        return with_tx<Workspace>([&](pqxx::work& tx) -> Result<Workspace> {
            const auto customers = tx.exec_params(
                "SELECT 1 FROM customers WHERE id = $1::uuid FOR UPDATE", input.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
            }
            const auto existing = tx.exec_params(
                "SELECT 1 FROM workspaces WHERE customer_id = $1::uuid AND name = $2 AND desired <> 'deleted'",
                input.customer_id, input.name);
            if (!existing.empty()) {
                return std::unexpected(StoreError{"conflict", "a workspace with this name already exists"});
            }
            const std::string id = new_uuid_v4();
            const std::string service_id = new_uuid_v4();
            tx.exec_params("INSERT INTO workspaces (id, customer_id, name, environment) "
                           "VALUES ($1::uuid, $2::uuid, $3, $4)", id, input.customer_id, input.name, input.environment);
            tx.exec_params("INSERT INTO services (id, customer_id, workspace_id, kind, name, slug, "
                           "is_default, created_by_taap_id) VALUES ($1::uuid, $2::uuid, $3::uuid, "
                           "'zetta', $4, $5, NOT EXISTS (SELECT 1 FROM services WHERE customer_id = $2::uuid "
                           "AND is_default), $6)",
                           service_id, input.customer_id, id, managed_service_slug("ws", id),
                           managed_service_slug("ws", id), input.created_by_taap_id);
            tx.exec_params("UPDATE workspaces SET zetta_service_id = $2::uuid WHERE id = $1::uuid", id, service_id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", service_id);
            return one_workspace(tx, id);
        });
    }

    Result<Workspace> mark_workspace_store_moved(const std::string& id) override {
        if (managed_service_slug("ws", id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must be a UUID"});
        }
        return with_tx<Workspace>([&](pqxx::work& tx) -> Result<Workspace> {
            tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid FOR UPDATE", id);
            const auto row = one_workspace(tx, id);
            if (!row) {
                return row;
            }
            if (row->desired == "deleted") {
                return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
            }
            if (!row->store_moved_at.empty()) {
                return row;
            }
            tx.exec_params("UPDATE workspaces SET store_moved_at = now() WHERE id = $1::uuid", id);
            return one_workspace(tx, id);
        });
    }

    Result<Workspace> update_workspace(const std::string& id, const WorkspacePatch& patch) override {
        if (managed_service_slug("ws", id).empty() ||
            (patch.name && (patch.name->empty() || patch.name->size() > 128)) ||
            (patch.environment && !valid_workspace_environment(*patch.environment)) ||
            (patch.desired && *patch.desired != "running" && *patch.desired != "stopped" && *patch.desired != "deleted")) {
            return std::unexpected(StoreError{"invalid_input", "invalid workspace_id, name, environment or desired"});
        }
        return with_tx<Workspace>([&](pqxx::work& tx) -> Result<Workspace> {
            tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid FOR UPDATE", id);
            const auto row = one_workspace(tx, id);
            if (!row) {
                return row;
            }
            if (row->desired == "deleted") {
                return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
            }
            const std::string name = patch.name.value_or(row->name);
            const std::string environment = patch.environment.value_or(row->environment);
            const std::string desired = patch.desired.value_or(row->desired);
            if (name == row->name && environment == row->environment && desired == row->desired) {
                return row;
            }
            tx.exec_params("UPDATE workspaces SET name = $2, environment = $3, desired = $4, "
                           "generation = generation + 1, updated_at = now() WHERE id = $1::uuid",
                           id, name, environment, desired);
            // Prevent the services controller from recreating a stopped or deleted workspace.
            tx.exec_params("UPDATE services SET desired = $2, generation = generation + 1, "
                           "updated_at = now() WHERE workspace_id = $1::uuid AND desired <> $2 "
                           "AND ($4 = 'deleted' OR id = $3::uuid)",
                           id, desired == "running" ? "present" : "absent", row->zetta_service_id, desired);
            if (desired == "deleted") {
                // Its services are deleted, so they mount nothing (lake#666).
                tx.exec_params("DELETE FROM store_mounts m USING stores s WHERE s.id = m.store_id "
                               "AND s.workspace_id = $1::uuid",
                               id);
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_workspace(tx, id);
        });
    }

    Result<Workspace> set_workspace_storage(const std::string& id, std::int32_t storage_gi) override {
        if (managed_service_slug("ws", id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
        }
        if (storage_gi < 1 || storage_gi > max_workspace_storage_gi) {
            return std::unexpected(StoreError{"invalid_input", "storage_gi is out of range"});
        }
        return with_tx<Workspace>([&](pqxx::work& tx) -> Result<Workspace> {
            tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid FOR UPDATE", id);
            const auto row = one_workspace(tx, id);
            if (!row) {
                return row;
            }
            if (row->desired == "deleted") {
                return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
            }
            if (storage_gi == row->storage_gi) {
                return row;
            }
            // enforce_workspace_storage_growth refuses a decrease (23514, invalid_input).
            tx.exec_params("UPDATE workspaces SET storage_gi = $2, generation = generation + 1, "
                           "updated_at = now() WHERE id = $1::uuid",
                           id, storage_gi);
            return one_workspace(tx, id);
        });
    }

    static constexpr const char* volume_snapshot_columns =
        "p.target, p.schedule, p.keep, "
        "COALESCE(to_char(p.requested_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), ''), "
        "to_char(p.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
        "to_char(p.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
        "st.target IS NOT NULL, COALESCE(st.snapshot_name, ''), COALESCE(st.ready_to_use, false), "
        "COALESCE(to_char(st.snapshot_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), ''), "
        "COALESCE(st.message, ''), "
        "COALESCE(to_char(st.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), '')";

    static VolumeSnapshotPolicy volume_snapshot_from(const pqxx::row& row) {
        VolumeSnapshotPolicy value;
        value.target = row[0].as<std::string>();
        value.schedule = row[1].as<std::string>();
        value.keep = row[2].as<std::int32_t>();
        value.requested_at = row[3].as<std::string>();
        value.created_at = row[4].as<std::string>();
        value.updated_at = row[5].as<std::string>();
        if (row[6].as<bool>()) {
            value.status = VolumeSnapshotStatus{row[7].as<std::string>(), row[8].as<bool>(),
                                                row[9].as<std::string>(), row[10].as<std::string>(),
                                                row[11].as<std::string>()};
        }
        return value;
    }

    Result<VolumeSnapshotPolicy> request_volume_snapshot(const VolumeSnapshotRequest& request) override {
        if (request.target != "store" && managed_service_slug("ws", request.target).empty()) {
            return std::unexpected(StoreError{"invalid_input", "target is unknown"});
        }
        if ((request.schedule && !valid_snapshot_schedule(*request.schedule)) ||
            (request.keep && (*request.keep < 1 || *request.keep > max_volume_snapshots_kept))) {
            return std::unexpected(StoreError{"invalid_input", "invalid schedule or keep"});
        }
        return with_tx<VolumeSnapshotPolicy>([&](pqxx::work& tx) -> Result<VolumeSnapshotPolicy> {
            if (request.target != "store" &&
                tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid AND desired <> 'deleted'",
                               request.target)
                    .empty()) {
                return std::unexpected(StoreError{"invalid_input", "target is unknown"});
            }
            const std::optional<std::string> schedule = request.schedule;
            const std::optional<std::int32_t> keep = request.keep;
            tx.exec_params("INSERT INTO volume_snapshot_policies (target, schedule, keep, requested_at) "
                           "VALUES ($1, COALESCE($2::text, '0 3 * * *'), COALESCE($3::integer, 7), now()) "
                           "ON CONFLICT (target) DO UPDATE SET "
                           "schedule = COALESCE($2::text, volume_snapshot_policies.schedule), "
                           "keep = COALESCE($3::integer, volume_snapshot_policies.keep), "
                           "requested_at = now(), updated_at = now()",
                           request.target, schedule, keep);
            const auto rows = tx.exec_params(std::string("SELECT ") + volume_snapshot_columns +
                                                 " FROM volume_snapshot_policies p LEFT JOIN "
                                                 "volume_snapshot_status st ON st.target = p.target "
                                                 "WHERE p.target = $1",
                                             request.target);
            return volume_snapshot_from(rows.at(0));
        });
    }

    Result<std::vector<VolumeSnapshotPolicy>> volume_snapshots() override {
        return with_tx<std::vector<VolumeSnapshotPolicy>>(
            [&](pqxx::work& tx) -> Result<std::vector<VolumeSnapshotPolicy>> {
                const auto rows = tx.exec(std::string("SELECT ") + volume_snapshot_columns +
                                          " FROM volume_snapshot_policies p LEFT JOIN "
                                          "volume_snapshot_status st ON st.target = p.target "
                                          "ORDER BY p.target <> 'store', p.target");
                std::vector<VolumeSnapshotPolicy> values;
                for (const auto& row : rows) {
                    values.push_back(volume_snapshot_from(row));
                }
                return values;
            });
    }

    static constexpr const char* workspace_deletion_columns =
        "workspace_id::text, customer_id::text, workspace_name, requested_by, "
        "to_char(requested_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
        "COALESCE(to_char(completed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), ''), "
        "repositories::text, apps::text, builds::text, images::text";

    static std::vector<WorkspaceDeletionItem> deletion_items(const std::string& text) {
        std::vector<WorkspaceDeletionItem> items;
        const nlohmann::json values = nlohmann::json::parse(text, nullptr, false);
        if (!values.is_array()) {
            return items;
        }
        for (const nlohmann::json& value : values) {
            if (value.is_object()) {
                items.push_back({value.value("id", std::string()), value.value("name", std::string()),
                                 value.value("state", std::string())});
            }
        }
        return items;
    }

    static WorkspaceDeletion workspace_deletion_from(const pqxx::row& row) {
        WorkspaceDeletion value;
        value.workspace_id = row[0].as<std::string>();
        value.customer_id = row[1].as<std::string>();
        value.workspace_name = row[2].as<std::string>();
        value.requested_by = row[3].as<std::string>();
        value.requested_at = row[4].as<std::string>();
        value.completed_at = row[5].as<std::string>();
        value.repositories = deletion_items(row[6].as<std::string>());
        value.apps = deletion_items(row[7].as<std::string>());
        value.builds = deletion_items(row[8].as<std::string>());
        value.images = deletion_items(row[9].as<std::string>());
        return value;
    }

    Result<WorkspaceDeletion> delete_workspace_cascade(const WorkspaceDeletionRequest& request) override {
        if (managed_service_slug("ws", request.workspace_id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must be a UUID"});
        }
        nlohmann::json images = nlohmann::json::array();
        for (const std::string& image : request.images) {
            images.push_back({{"id", ""}, {"name", image}, {"state", "tagged"}});
        }
        return with_tx<WorkspaceDeletion>([&](pqxx::work& tx) -> Result<WorkspaceDeletion> {
            tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid FOR UPDATE",
                           request.workspace_id);
            const auto row = one_workspace(tx, request.workspace_id);
            if (!row) {
                return std::unexpected(row.error());
            }
            if (row->desired == "deleted") {
                const auto recorded = tx.exec_params(
                    std::string("SELECT ") + workspace_deletion_columns +
                        " FROM workspace_deletions WHERE workspace_id = $1::uuid",
                    request.workspace_id);
                if (recorded.empty()) {
                    return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
                }
                return workspace_deletion_from(recorded[0]);
            }
            // Repositories still live or inside their retention: tombstoned now with retention
            // ended, which releases their names. Expired tombstones are already gone.
            const auto repositories = tx.exec_params(
                "WITH destroyed AS (SELECT id, name, deleted_at IS NULL AS live FROM repositories "
                "WHERE workspace_id = $1::uuid AND (deleted_at IS NULL OR retention_until > now()) "
                "FOR UPDATE), changed AS (UPDATE repositories r SET deleted_at = "
                "COALESCE(r.deleted_at, now()), retention_until = now() FROM destroyed d "
                "WHERE r.id = d.id RETURNING r.id) "
                "SELECT COALESCE(jsonb_agg(jsonb_build_object('id', d.id::text, 'name', d.name, "
                "'state', CASE WHEN d.live THEN 'live' ELSE 'tombstoned' END) ORDER BY d.name), "
                "'[]'::jsonb)::text FROM destroyed d",
                request.workspace_id);
            const auto apps = tx.exec_params(
                "WITH changed AS (UPDATE apps a SET desired = 'deleted', generation = generation + 1, "
                "updated_at = now() FROM (SELECT id, desired FROM apps WHERE workspace_id = $1::uuid "
                "AND desired <> 'deleted' FOR UPDATE) b WHERE a.id = b.id "
                "RETURNING a.id, a.name, b.desired AS previous) "
                "SELECT COALESCE(jsonb_agg(jsonb_build_object('id', id::text, 'name', name, "
                "'state', previous) ORDER BY name), '[]'::jsonb)::text FROM changed",
                request.workspace_id);
            const auto builds = tx.exec_params(
                "WITH changed AS (UPDATE builds SET desired = 'cancelled', updated_at = now() "
                "WHERE workspace_id = $1::uuid AND desired = 'queued' AND phase NOT IN "
                "('Succeeded', 'Failed', 'Cancelled') RETURNING id, target, phase) "
                "SELECT COALESCE(jsonb_agg(jsonb_build_object('id', id::text, 'name', target, "
                "'state', phase) ORDER BY id), '[]'::jsonb)::text FROM changed",
                request.workspace_id);
            // lake#383: the workspace's tenant image routes go in this transaction; their
            // manifests and tags go with its Zetta.
            const auto routes = tx.exec_params(
                "WITH gone AS (DELETE FROM oci_repository_routes WHERE workspace_id = $1::uuid "
                "RETURNING customer_id, name) SELECT c.name || '/' || g.name FROM gone g "
                "JOIN customers c ON c.id = g.customer_id ORDER BY 1",
                request.workspace_id);
            for (const auto& route : routes) {
                const std::string image = route[0].as<std::string>();
                if (std::ranges::find(request.images, image) == request.images.end()) {
                    images.push_back({{"id", ""}, {"name", image}, {"state", "routed"}});
                }
            }
            {
                std::vector<nlohmann::json> sorted(images.begin(), images.end());
                std::ranges::sort(sorted, {}, [](const nlohmann::json& image) {
                    return image.value("name", std::string());
                });
                images = sorted;
            }
            tx.exec_params("UPDATE workspaces SET desired = 'deleted', generation = generation + 1, "
                           "updated_at = now() WHERE id = $1::uuid",
                           request.workspace_id);
            tx.exec_params("UPDATE services SET desired = 'absent', generation = generation + 1, "
                           "updated_at = now() WHERE workspace_id = $1::uuid AND desired <> 'absent'",
                           request.workspace_id);
            // Its services and apps are deleted, so they mount nothing (lake#666).
            tx.exec_params("DELETE FROM store_mounts m USING stores s WHERE s.id = m.store_id "
                           "AND s.workspace_id = $1::uuid",
                           request.workspace_id);
            const auto inserted = tx.exec_params(
                std::string("INSERT INTO workspace_deletions (workspace_id, customer_id, "
                            "workspace_name, requested_by, repositories, apps, builds, images) "
                            "VALUES ($1::uuid, $2::uuid, $3, $4, $5::jsonb, $6::jsonb, $7::jsonb, "
                            "$8::jsonb) RETURNING ") +
                    workspace_deletion_columns,
                request.workspace_id, row->customer_id, row->name, request.requested_by,
                repositories[0][0].as<std::string>(), apps[0][0].as<std::string>(),
                builds[0][0].as<std::string>(), images.dump());
            for (const char* channel : {"lake_apps", "lake_builds"}) {
                const pqxx::result& changed = std::string_view(channel) == "lake_apps" ? apps : builds;
                const auto ids = nlohmann::json::parse(changed[0][0].as<std::string>(), nullptr, false);
                if (!ids.is_array()) {
                    continue;
                }
                for (const nlohmann::json& id : ids) {
                    tx.exec_params("SELECT pg_notify($1, $2)", channel,
                                   id.value("id", std::string()));
                }
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", request.workspace_id);
            return workspace_deletion_from(inserted[0]);
        });
    }

    static OciRepositoryRoute oci_route_from(const pqxx::row& row) {
        return {row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                row[3].as<std::string>(), row[4].as<std::string>()};
    }

    static constexpr const char* oci_route_columns =
        "customer_id::text, name, workspace_id::text, "
        "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
        "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')";

    Result<OciRepositoryRoute> create_oci_repository_route(const std::string& customer_id,
                                                           const std::string& name,
                                                           const std::string& workspace_id) override {
        if (!valid_oci_route_name(name)) {
            return std::unexpected(
                StoreError{"invalid_input", "name is not a valid image repository name"});
        }
        if (managed_service_slug("ws", workspace_id).empty() ||
            managed_service_slug("ws", customer_id).empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "workspace_id must name a live workspace of this account"});
        }
        return with_tx<OciRepositoryRoute>([&](pqxx::work& tx) -> Result<OciRepositoryRoute> {
            const auto workspace = tx.exec_params(
                "SELECT 1 FROM workspaces WHERE id = $1::uuid AND customer_id = $2::uuid "
                "AND desired <> 'deleted' FOR SHARE",
                workspace_id, customer_id);
            if (workspace.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "workspace_id must name a live workspace of this account"});
            }
            tx.exec_params("INSERT INTO oci_repository_routes (customer_id, name, workspace_id) "
                           "VALUES ($1::uuid, $2, $3::uuid) ON CONFLICT (customer_id, name) "
                           "DO NOTHING",
                           customer_id, name, workspace_id);
            const auto rows = tx.exec_params(std::string("SELECT ") + oci_route_columns +
                                                 " FROM oci_repository_routes WHERE customer_id = "
                                                 "$1::uuid AND name = $2",
                                             customer_id, name);
            if (rows.empty()) {
                return std::unexpected(StoreError{"store_error", "the image route was not kept"});
            }
            OciRepositoryRoute route = oci_route_from(rows[0]);
            if (route.workspace_id != workspace_id) {
                return std::unexpected(StoreError{
                    "conflict", "image repository " + name + " belongs to another workspace"});
            }
            return route;
        });
    }

    Result<OciRepositoryRoute> get_oci_repository_route(const std::string& customer_id,
                                                        const std::string& name) override {
        if (managed_service_slug("ws", customer_id).empty()) {
            return std::unexpected(StoreError{"not_found", "no image repository " + name});
        }
        return with_tx<OciRepositoryRoute>([&](pqxx::work& tx) -> Result<OciRepositoryRoute> {
            const auto rows = tx.exec_params(std::string("SELECT ") + oci_route_columns +
                                                 " FROM oci_repository_routes WHERE customer_id = "
                                                 "$1::uuid AND name = $2",
                                             customer_id, name);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no image repository " + name});
            }
            return oci_route_from(rows[0]);
        });
    }

    Result<std::vector<OciRepositoryRoute>>
    list_oci_repository_routes(const std::string& customer_id,
                               const std::string& workspace_id) override {
        if (managed_service_slug("ws", customer_id).empty() ||
            (!workspace_id.empty() && managed_service_slug("ws", workspace_id).empty())) {
            return std::vector<OciRepositoryRoute>{};
        }
        return with_tx<std::vector<OciRepositoryRoute>>(
            [&](pqxx::work& tx) -> Result<std::vector<OciRepositoryRoute>> {
                const auto rows = tx.exec_params(
                    std::string("SELECT ") + oci_route_columns +
                        " FROM oci_repository_routes WHERE customer_id = $1::uuid AND "
                        "(NULLIF($2, '') IS NULL OR workspace_id = NULLIF($2, '')::uuid) "
                        "ORDER BY name",
                    customer_id, workspace_id);
                std::vector<OciRepositoryRoute> routes;
                for (const auto& row : rows) {
                    routes.push_back(oci_route_from(row));
                }
                return routes;
            });
    }

    Result<std::vector<OciRepositoryRoute>>
    delete_oci_repository_route(const std::string& customer_id, const std::string& name,
                                const std::string& to_workspace_id) override {
        if (managed_service_slug("ws", customer_id).empty()) {
            return std::unexpected(StoreError{"not_found", "no image repository " + name});
        }
        if (!to_workspace_id.empty() && managed_service_slug("ws", to_workspace_id).empty()) {
            return std::unexpected(StoreError{
                "invalid_input", "workspace_id must name a live workspace of this account"});
        }
        const std::string cache = name.starts_with("cache/") ? name : "cache/" + name;
        return with_tx<std::vector<OciRepositoryRoute>>(
            [&](pqxx::work& tx) -> Result<std::vector<OciRepositoryRoute>> {
                if (!to_workspace_id.empty() &&
                    tx.exec_params("SELECT 1 FROM workspaces WHERE id = $1::uuid AND customer_id = "
                                   "$2::uuid AND desired <> 'deleted' FOR SHARE",
                                   to_workspace_id, customer_id)
                        .empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "workspace_id must name a live workspace of this account"});
                }
                // Deleted and written again in this transaction: a concurrent first push waits on
                // the deleted keys and then finds the new route.
                const auto rows = tx.exec_params(
                    std::string("DELETE FROM oci_repository_routes WHERE customer_id = $1::uuid "
                                "AND name IN ($2, $3) RETURNING ") +
                        oci_route_columns,
                    customer_id, name, cache);
                std::vector<OciRepositoryRoute> removed;
                for (const auto& row : rows) {
                    removed.push_back(oci_route_from(row));
                }
                std::ranges::sort(removed, [&](const auto& left, const auto& right) {
                    return (left.name == name) > (right.name == name);
                });
                if (removed.empty() || removed.front().name != name) {
                    return std::unexpected(StoreError{"not_found", "no image repository " + name});
                }
                if (!to_workspace_id.empty()) {
                    for (const OciRepositoryRoute& route : removed) {
                        tx.exec_params("INSERT INTO oci_repository_routes (customer_id, name, "
                                       "workspace_id) VALUES ($1::uuid, $2, $3::uuid)",
                                       customer_id, route.name, to_workspace_id);
                    }
                }
                return removed;
            });
    }

    Result<WorkspaceDeletion> get_workspace_deletion(const std::string& workspace_id) override {
        if (managed_service_slug("ws", workspace_id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must be a UUID"});
        }
        return with_tx<WorkspaceDeletion>([&](pqxx::work& tx) -> Result<WorkspaceDeletion> {
            const auto rows = tx.exec_params(std::string("SELECT ") + workspace_deletion_columns +
                                                 " FROM workspace_deletions WHERE workspace_id = $1::uuid",
                                             workspace_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no deletion of workspace " + workspace_id});
            }
            return workspace_deletion_from(rows[0]);
        });
    }

    Result<std::vector<WorkspaceDeletion>> pending_workspace_deletions(int limit) override {
        return with_tx<std::vector<WorkspaceDeletion>>(
            [&](pqxx::work& tx) -> Result<std::vector<WorkspaceDeletion>> {
                const auto rows = tx.exec_params(
                    std::string("SELECT ") + workspace_deletion_columns +
                        " FROM workspace_deletions WHERE completed_at IS NULL "
                        "ORDER BY requested_at, workspace_id LIMIT $1",
                    limit > 0 ? limit : 1000);
                std::vector<WorkspaceDeletion> values;
                for (const auto& row : rows) {
                    values.push_back(workspace_deletion_from(row));
                }
                return values;
            });
    }

    Result<void> complete_workspace_deletion(const std::string& workspace_id) override {
        if (managed_service_slug("ws", workspace_id).empty()) {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must be a UUID"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const auto rows = tx.exec_params(
                "UPDATE workspace_deletions SET completed_at = COALESCE(completed_at, now()) "
                "WHERE workspace_id = $1::uuid RETURNING 1",
                workspace_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no deletion of workspace " + workspace_id});
            }
            return {};
        });
    }

    static Result<void> check_network_workspace(pqxx::work& tx, const NetworkScope& scope) {
        const auto rows = tx.exec_params(
            "SELECT 1 FROM workspaces WHERE id = $1::uuid AND customer_id = $2::uuid "
            "AND desired <> 'deleted' FOR SHARE",
            scope.workspace_id,
            scope.customer_id);
        if (rows.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "no live workspace in this customer"});
        }
        return {};
    }

    static Result<void> check_network_target(pqxx::work& tx,
                                             const NetworkScope& scope,
                                             const std::string& kind,
                                             const std::string& id) {
        // These are fixed SQL fragments, never caller-supplied identifiers.
        std::string source;
        if (kind == "app") {
            source = "apps WHERE desired <> 'deleted'";
        } else if (kind == "service") {
            source = "services WHERE desired = 'present'";
        } else if (kind == "agent") {
            source = "agent_launches WHERE desired = 'present'";
        } else if (kind == "tunnel") {
            source = "network_tunnel_grants WHERE revoked_at IS NULL AND expires_at > now()";
        } else {
            return std::unexpected(StoreError{"invalid_input", "unknown network workload kind"});
        }
        const auto rows = tx.exec_params("SELECT 1 FROM " + source +
                                             " AND id = $1::uuid AND customer_id = $2::uuid AND "
                                             "workspace_id = $3::uuid FOR SHARE",
                                         id,
                                         scope.customer_id,
                                         scope.workspace_id);
        if (rows.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "network workload is not live in this workspace"});
        }
        return {};
    }

    static std::string network_select(const network_detail::Table& table) {
        std::string value = "to_jsonb(n)";
        if (table.tunnel) {
            value +=
                " || jsonb_build_object('expires_at', "
                "to_char(n.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
                "'expired', n.expires_at <= now())";
        }
        return "SELECT (" + value + " || jsonb_build_object('status', to_jsonb(s)))::text FROM " +
               table.name + " n LEFT JOIN " + table.status + " s ON s." + table.status_key +
               " = n.id WHERE n.customer_id = $1::uuid AND n.workspace_id = $2::uuid";
    }

    template <class Row>
    static Result<Row>
    one_network(pqxx::work& tx, const NetworkScope& scope, const std::string& id) {
        const auto rows =
            tx.exec_params(network_select(network_detail::table<Row>()) + " AND n.id = $3::uuid",
                           scope.customer_id,
                           scope.workspace_id,
                           id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no network record in this workspace"});
        }
        return network_detail::decode<Row>(
            nlohmann::json::parse(rows[0][0].template as<std::string>()));
    }

    template <class Row> Result<Row> get_network(const NetworkScope& scope, const std::string& id) {
        if (auto checked = network_detail::validate_scope(scope); !checked) {
            return std::unexpected(checked.error());
        }
        if (!network_detail::uuid(id)) {
            return std::unexpected(StoreError{"invalid_input", "network id must be a UUID"});
        }
        return with_tx<Row>([&](pqxx::work& tx) { return one_network<Row>(tx, scope, id); });
    }

    template <class Row> Result<std::vector<Row>> list_network(const NetworkScope& scope) {
        if (auto checked = network_detail::validate_scope(scope); !checked) {
            return std::unexpected(checked.error());
        }
        return with_tx<std::vector<Row>>([&](pqxx::work& tx) -> Result<std::vector<Row>> {
            const auto rows = tx.exec_params(network_select(network_detail::table<Row>()) +
                                                 " ORDER BY n.created_at, n.id",
                                             scope.customer_id,
                                             scope.workspace_id);
            std::vector<Row> result;
            for (const auto& row : rows) {
                result.push_back(network_detail::decode<Row>(
                    nlohmann::json::parse(row[0].template as<std::string>())));
            }
            return result;
        });
    }

    static Result<void> write_network_spec(pqxx::work& tx,
                                           const network_detail::Table& table,
                                           const NetworkScope& scope,
                                           const std::string& id,
                                           const nlohmann::json& spec,
                                           const NetworkActor& actor,
                                           bool create) {
        std::string columns;
        std::string values;
        std::string assignments;
        for (const auto& field : table.fields) {
            if (!columns.empty()) {
                columns += ", ";
                values += ", ";
                assignments += ", ";
            }
            columns += field;
            values += "v." + field;
            assignments += field + " = v." + field;
        }
        const std::string decoded =
            "jsonb_populate_record(NULL::" + std::string(table.name) + ", $4::jsonb) v";
        if (create) {
            tx.exec_params("INSERT INTO " + std::string(table.name) +
                               " (id, customer_id, workspace_id, created_by, " + columns +
                               ") SELECT $1::uuid, $2::uuid, $3::uuid, NULLIF($5, '')::uuid, " +
                               values + " FROM " + decoded,
                           id,
                           scope.customer_id,
                           scope.workspace_id,
                           spec.dump(),
                           actor.user_id);
        } else {
            tx.exec_params("UPDATE " + std::string(table.name) + " n SET " + assignments +
                               ", generation = n.generation + 1, updated_at = now() FROM " +
                               decoded +
                               " WHERE n.id = $1::uuid AND n.customer_id = $2::uuid AND "
                               "n.workspace_id = $3::uuid",
                           id,
                           scope.customer_id,
                           scope.workspace_id,
                           spec.dump());
        }
        return {};
    }

    template <class Row>
    Result<Row> change_network(const NetworkScope& scope,
                               const std::string& supplied_id,
                               const nlohmann::json& spec,
                               const NetworkActor& actor,
                               const std::string& operation) {
        if (auto checked = network_detail::validate_scope(scope); !checked) {
            return std::unexpected(checked.error());
        }
        const bool create = operation == "create";
        if (!create && !network_detail::uuid(supplied_id)) {
            return std::unexpected(StoreError{"invalid_input", "network id must be a UUID"});
        }
        const audit::IdentityOptions options{.instance_kek = instance_kek_};
        auto signer = network_detail::prepare_audit(*this, actor, options);
        if (!signer) {
            return std::unexpected(signer.error());
        }
        const std::string id = create ? new_uuid_v4() : supplied_id;
        return with_tx<Row>([&](pqxx::work& tx) -> Result<Row> {
            const auto& table = network_detail::table<Row>();
            if (auto checked = check_network_workspace(tx, scope); !checked) {
                return std::unexpected(checked.error());
            }
            if (!create) {
                const auto rows =
                    tx.exec_params(network_select(table) + " AND n.id = $3::uuid FOR UPDATE OF n",
                                   scope.customer_id,
                                   scope.workspace_id,
                                   id);
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "no network record in this workspace"});
                }
                const auto previous = nlohmann::json::parse(rows[0][0].template as<std::string>());
                const bool ended = table.tunnel ? !previous.at("revoked_at").is_null() ||
                                                      previous.at("expired").template get<bool>()
                                                : previous.at("desired") != "present";
                if (ended) {
                    return std::unexpected(StoreError{"conflict", "the network record has ended"});
                }
            }
            if (create || operation == "update") {
                auto checked = network_detail::validate(
                    table, spec, [&](const std::string& kind, const std::string& target) {
                        return check_network_target(tx, scope, kind, target);
                    });
                if (!checked) {
                    return std::unexpected(checked.error());
                }
                if (table.tunnel && tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid",
                                                   spec.at("user_id").get<std::string>())
                                        .empty()) {
                    return std::unexpected(StoreError{"invalid_input", "no tunnel user"});
                }
                write_network_spec(tx, table, scope, id, spec, actor, create);
            } else {
                tx.exec_params(
                    "UPDATE " + std::string(table.name) + " SET " +
                        (table.tunnel ? "revoked_at = now()" : "desired = 'absent'") +
                        ", generation = generation + 1, updated_at = now() WHERE id = $1::uuid",
                    id);
            }
            auto result = one_network<Row>(tx, scope, id);
            if (!result) {
                return result;
            }
            tx.exec("SELECT pg_advisory_xact_lock(hashtext('lake_audit_events'))");
            const auto previous =
                tx.exec("SELECT digest FROM audit_events ORDER BY sequence DESC LIMIT 1");
            auto event = network_detail::sign_change(
                *this,
                *signer,
                actor,
                scope,
                table,
                id,
                operation,
                result->generation,
                previous.empty() ? "" : previous[0][0].as<std::string>(),
                options);
            if (!event) {
                return std::unexpected(event.error());
            }
            if (auto appended = append_audit_tx(tx, *event); !appended) {
                return std::unexpected(appended.error());
            }
            return result;
        });
    }

    Result<NetworkConnection> create_network_connection(const NetworkScope& scope,
                                                        const NetworkConnectionSpec& spec,
                                                        const NetworkActor& actor) override {
        return change_network<NetworkConnection>(
            scope, "", network_detail::encode(spec), actor, "create");
    }

    Result<NetworkConnection> update_network_connection(const NetworkScope& scope,
                                                        const std::string& id,
                                                        const NetworkConnectionSpec& spec,
                                                        const NetworkActor& actor) override {
        return change_network<NetworkConnection>(
            scope, id, network_detail::encode(spec), actor, "update");
    }

    Result<NetworkConnection> withdraw_network_connection(const NetworkScope& scope,
                                                          const std::string& id,
                                                          const NetworkActor& actor) override {
        return change_network<NetworkConnection>(scope, id, nullptr, actor, "withdraw");
    }

    Result<NetworkConnection> get_network_connection(const NetworkScope& scope,
                                                     const std::string& id) override {
        return get_network<NetworkConnection>(scope, id);
    }

    Result<std::vector<NetworkConnection>>
    list_network_connections(const NetworkScope& scope) override {
        return list_network<NetworkConnection>(scope);
    }

    Result<NetworkEgressRule> create_network_egress_rule(const NetworkScope& scope,
                                                         const NetworkEgressRuleSpec& spec,
                                                         const NetworkActor& actor) override {
        return change_network<NetworkEgressRule>(
            scope, "", network_detail::encode(spec), actor, "create");
    }

    Result<NetworkEgressRule> update_network_egress_rule(const NetworkScope& scope,
                                                         const std::string& id,
                                                         const NetworkEgressRuleSpec& spec,
                                                         const NetworkActor& actor) override {
        return change_network<NetworkEgressRule>(
            scope, id, network_detail::encode(spec), actor, "update");
    }

    Result<NetworkEgressRule> withdraw_network_egress_rule(const NetworkScope& scope,
                                                           const std::string& id,
                                                           const NetworkActor& actor) override {
        return change_network<NetworkEgressRule>(scope, id, nullptr, actor, "withdraw");
    }

    Result<NetworkEgressRule> get_network_egress_rule(const NetworkScope& scope,
                                                      const std::string& id) override {
        return get_network<NetworkEgressRule>(scope, id);
    }

    Result<std::vector<NetworkEgressRule>>
    list_network_egress_rules(const NetworkScope& scope) override {
        return list_network<NetworkEgressRule>(scope);
    }

    Result<NetworkTunnelGrant> create_network_tunnel_grant(const NetworkScope& scope,
                                                           const NetworkTunnelGrantSpec& spec,
                                                           const NetworkActor& actor) override {
        return change_network<NetworkTunnelGrant>(
            scope, "", network_detail::encode(spec), actor, "create");
    }

    Result<NetworkTunnelGrant> update_network_tunnel_grant(const NetworkScope& scope,
                                                           const std::string& id,
                                                           const NetworkTunnelGrantSpec& spec,
                                                           const NetworkActor& actor) override {
        return change_network<NetworkTunnelGrant>(
            scope, id, network_detail::encode(spec), actor, "update");
    }

    Result<NetworkTunnelGrant> revoke_network_tunnel_grant(const NetworkScope& scope,
                                                           const std::string& id,
                                                           const NetworkActor& actor) override {
        return change_network<NetworkTunnelGrant>(scope, id, nullptr, actor, "revoke");
    }

    Result<NetworkTunnelGrant> get_network_tunnel_grant(const NetworkScope& scope,
                                                        const std::string& id) override {
        return get_network<NetworkTunnelGrant>(scope, id);
    }

    Result<std::vector<NetworkTunnelGrant>>
    list_network_tunnel_grants(const NetworkScope& scope) override {
        return list_network<NetworkTunnelGrant>(scope);
    }

    Result<NetworkPeer> create_network_peer(const NetworkScope& scope,
                                            const NetworkPeerSpec& spec,
                                            const NetworkActor& actor) override {
        return change_network<NetworkPeer>(
            scope, "", network_detail::encode(spec), actor, "create");
    }

    Result<NetworkPeer> update_network_peer(const NetworkScope& scope,
                                            const std::string& id,
                                            const NetworkPeerSpec& spec,
                                            const NetworkActor& actor) override {
        return change_network<NetworkPeer>(
            scope, id, network_detail::encode(spec), actor, "update");
    }

    Result<NetworkPeer> withdraw_network_peer(const NetworkScope& scope,
                                              const std::string& id,
                                              const NetworkActor& actor) override {
        return change_network<NetworkPeer>(scope, id, nullptr, actor, "withdraw");
    }

    Result<NetworkPeer> get_network_peer(const NetworkScope& scope,
                                         const std::string& id) override {
        return get_network<NetworkPeer>(scope, id);
    }

    Result<std::vector<NetworkPeer>> list_network_peers(const NetworkScope& scope) override {
        return list_network<NetworkPeer>(scope);
    }

    Result<std::vector<ManagedService>> list_services(const std::string& customer_id) override {
        return with_tx<std::vector<ManagedService>>(
            [&](pqxx::work& tx) -> Result<std::vector<ManagedService>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + service_columns +
                        " FROM services s LEFT JOIN service_status st ON st.service_id = s.id "
                        // A deleted workspace's primary Zetta is gone once the operator reports
                        // the workspace Deleted, whether or not it mirrored that into
                        // service_status (lake#452).
                        "LEFT JOIN workspaces w ON w.id = s.workspace_id "
                        "WHERE s.customer_id = $1::uuid AND NOT (s.desired = 'absent' AND "
                        "COALESCE(st.phase, '') = 'Deleted') AND NOT (s.desired = 'absent' AND "
                        "w.zetta_service_id = s.id AND w.desired = 'deleted' AND "
                        "COALESCE(w.phase, '') = 'Deleted') ORDER BY s.created_at, s.id",
                    customer_id);
                std::vector<ManagedService> services;
                for (const pqxx::row& row : rows) {
                    services.push_back(service_from(row));
                }
                return services;
            });
    }

    Result<ManagedService> get_service(const std::string& id) override {
        return with_tx<ManagedService>(
            [&](pqxx::work& tx) -> Result<ManagedService> { return one_service(tx, id); });
    }

    Result<ManagedService> create_service(const NewManagedService& in) override {
        return with_tx<ManagedService>([&](pqxx::work& tx) -> Result<ManagedService> {
            // One writer at a time per customer, so the count and the name check hold.
            const pqxx::result customers = tx.exec_params(
                "SELECT 1 FROM customers WHERE id = $1::uuid FOR UPDATE", in.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
            }
            if (in.is_default) {
                const pqxx::result existing = tx.exec_params(
                    "SELECT id::text FROM services WHERE customer_id = $1::uuid AND is_default "
                    "AND desired = 'present'",
                    in.customer_id);
                if (!existing.empty()) {
                    return one_service(tx, existing[0][0].as<std::string>());
                }
            }
            const pqxx::result named = tx.exec_params(
                "SELECT 1 FROM services WHERE customer_id = $1::uuid AND name = $2 AND desired = "
                "'present'",
                in.customer_id,
                in.name);
            if (!named.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "a service named " + in.name + " already exists"});
            }
            const pqxx::result live = tx.exec_params(
                "SELECT count(*) FROM services WHERE customer_id = $1::uuid AND desired = "
                "'present'",
                in.customer_id);
            if (in.max_per_customer != 0 && live[0][0].as<std::size_t>() >= in.max_per_customer) {
                return std::unexpected(StoreError{"quota_exceeded",
                                                  "this account already runs " +
                                                      std::to_string(in.max_per_customer) +
                                                      " services"});
            }
            const std::string id = new_uuid_v4();
            std::string slug = managed_service_slug("svc", id);
            if (in.is_default) {
                // The first workspace keeps decision 45's name so the operator adopts its
                // cluster; a slug is never reused, so a later one gets a service slug.
                const std::string workspace = managed_service_slug("ws", in.customer_id);
                const pqxx::result taken =
                    tx.exec_params("SELECT 1 FROM services WHERE slug = $1", workspace);
                if (taken.empty()) {
                    slug = workspace;
                }
            }
            const nlohmann::json spec{{"instances", in.instances}, {"storage_gi", in.storage_gi}};
            tx.exec_params("INSERT INTO services (id, customer_id, kind, name, slug, is_default, "
                           "spec, created_by_taap_id, workspace_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, "
                           "$6, $7::jsonb, $8, NULLIF($9,'')::uuid)",
                           id,
                           in.customer_id,
                           in.kind,
                           in.name,
                           slug,
                           in.is_default,
                           spec.dump(),
                           in.created_by_taap_id,
                           in.workspace_id);
            if (!in.stores.empty()) {
                if (auto written =
                        write_store_mounts(tx, "service", id, in.workspace_id, in.stores);
                    !written) {
                    return std::unexpected(written.error());
                }
            }
            // The operator LISTENs and reconciles at once instead of on its next poll (lake#197);
            // delivered on commit.
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_service(tx, id);
        });
    }

    Result<ManagedService> withdraw_service(const std::string& id) override {
        return with_tx<ManagedService>([&](pqxx::work& tx) -> Result<ManagedService> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE services SET desired = 'absent', generation = generation + 1, "
                "updated_at = now() WHERE id = $1::uuid AND desired = 'present' RETURNING id",
                id);
            if (rows.empty()) {
                const Result<ManagedService> existing = one_service(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(StoreError{"conflict", "the service is already deleted"});
            }
            // A deleted service mounts nothing (lake#666).
            tx.exec_params(
                "DELETE FROM store_mounts WHERE object_kind = 'service' AND object_id = $1::uuid",
                id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_service(tx, id);
        });
    }

    Result<ManagedService>
    set_service_stores(const std::string& service_id,
                       const std::vector<StoreMountRequest>& mounts) override {
        return with_tx<ManagedService>([&](pqxx::work& tx) -> Result<ManagedService> {
            const pqxx::result rows =
                tx.exec_params("SELECT desired, COALESCE(workspace_id::text, '') FROM services "
                               "WHERE id = $1::uuid FOR UPDATE",
                               service_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no service " + service_id});
            }
            if (rows[0][0].as<std::string>() != "present") {
                return std::unexpected(StoreError{"conflict", "the service is being deleted"});
            }
            if (auto written = write_store_mounts(
                    tx, "service", service_id, rows[0][1].as<std::string>(), mounts);
                !written) {
                return std::unexpected(written.error());
            }
            tx.exec_params("UPDATE services SET generation = generation + 1, updated_at = now() "
                           "WHERE id = $1::uuid",
                           service_id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", service_id);
            return one_service(tx, service_id);
        });
    }

    Result<std::vector<StoreMount>> list_store_mounts(const std::string& customer_id,
                                                      const std::string& workspace_id) override {
        return with_tx<std::vector<StoreMount>>(
            [&](pqxx::work& tx) -> Result<std::vector<StoreMount>> {
                return store_mounts_of(tx, customer_id, workspace_id, "");
            });
    }

    // The mounts of the customer's stores (or of one store), with the store's and the workload's
    // names. A mount whose workload row is gone keeps an empty name.
    static std::vector<StoreMount> store_mounts_of(pqxx::work& tx,
                                                   const std::string& customer_id,
                                                   const std::string& workspace_id,
                                                   const std::string& store_id) {
        const pqxx::result rows = tx.exec_params(
            "SELECT m.store_id::text, m.object_kind, m.object_id::text, m.mount_path, m.read_only, "
            "to_char(m.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
            "s.name, s.type, s.provider, COALESCE(sv.name, a.name, ''), "
            "COALESCE(s.workspace_id::text, '') "
            "FROM store_mounts m JOIN stores s ON s.id = m.store_id "
            "LEFT JOIN services sv ON m.object_kind = 'service' AND sv.id = m.object_id "
            "LEFT JOIN apps a ON m.object_kind = 'app' AND a.id = m.object_id "
            "WHERE ($1 = '' OR s.customer_id = NULLIF($1, '')::uuid) "
            "AND ($2 = '' OR s.workspace_id = NULLIF($2, '')::uuid) "
            "AND ($3 = '' OR m.store_id = NULLIF($3, '')::uuid) "
            "ORDER BY s.name, m.object_kind, COALESCE(sv.name, a.name, ''), m.object_id",
            customer_id,
            workspace_id,
            store_id);
        std::vector<StoreMount> mounts;
        for (const pqxx::row& row : rows) {
            StoreMount mount;
            mount.store_id = row[0].as<std::string>();
            mount.object_kind = row[1].as<std::string>();
            mount.object_id = row[2].as<std::string>();
            mount.mount_path = row[3].as<std::string>();
            mount.read_only = row[4].as<bool>();
            mount.created_at = row[5].as<std::string>();
            mount.store_name = row[6].as<std::string>();
            mount.store_type = row[7].as<std::string>();
            mount.store_provider = row[8].as<std::string>();
            mount.workload_name = row[9].as<std::string>();
            mount.workspace_id = row[10].as<std::string>();
            mounts.push_back(std::move(mount));
        }
        return mounts;
    }

    // Replaces what one workload mounts (lake#666). Each store row is locked first, so two writers
    // mounting one volume, or a mount racing delete_store, take turns: a store must be present and
    // in the workload's workspace, and a volume (ReadWriteOnce) is mounted by one workload only.
    static Result<void> write_store_mounts(pqxx::work& tx,
                                           const std::string& object_kind,
                                           const std::string& object_id,
                                           const std::string& workspace_id,
                                           const std::vector<StoreMountRequest>& mounts) {
        for (const StoreMountRequest& mount : mounts) {
            const pqxx::result stores = tx.exec_params(
                "SELECT name, type, desired, COALESCE(workspace_id::text, '') FROM stores "
                "WHERE id = $1::uuid FOR UPDATE",
                mount.store_id);
            if (stores.empty()) {
                return std::unexpected(StoreError{"not_found", "no store " + mount.store_id});
            }
            const std::string name = stores[0][0].as<std::string>();
            if (stores[0][2].as<std::string>() != "present") {
                return std::unexpected(
                    StoreError{"invalid_input", "the store " + name + " is being deleted"});
            }
            if (stores[0][3].as<std::string>() != workspace_id) {
                return std::unexpected(StoreError{
                    "invalid_input", "the store " + name + " belongs to another workspace"});
            }
            if (stores[0][1].as<std::string>() != "volume") {
                continue;
            }
            const pqxx::result others = tx.exec_params(
                "SELECT m.object_kind, COALESCE(sv.name, a.name, m.object_id::text) "
                "FROM store_mounts m "
                "LEFT JOIN services sv ON m.object_kind = 'service' AND sv.id = m.object_id "
                "LEFT JOIN apps a ON m.object_kind = 'app' AND a.id = m.object_id "
                "WHERE m.store_id = $1::uuid AND NOT (m.object_kind = $2 AND m.object_id = "
                "$3::uuid)",
                mount.store_id,
                object_kind,
                object_id);
            if (!others.empty()) {
                return std::unexpected(StoreError{
                    "conflict",
                    "the volume " + name + " is already mounted by the " +
                        others[0][0].as<std::string>() + " " + others[0][1].as<std::string>() +
                        "; a volume is mounted by one service or app"});
            }
        }
        tx.exec_params("DELETE FROM store_mounts WHERE object_kind = $1 AND object_id = $2::uuid",
                       object_kind,
                       object_id);
        for (const StoreMountRequest& mount : mounts) {
            tx.exec_params(
                "INSERT INTO store_mounts (store_id, object_kind, object_id, mount_path, "
                "read_only) VALUES ($1::uuid, $2, $3::uuid, $4, $5)",
                mount.store_id,
                object_kind,
                object_id,
                mount.mount_path,
                mount.read_only);
        }
        return {};
    }

    static constexpr const char* web_app_columns =
        "a.id::text, a.customer_id::text, a.name, a.mode, a.source_repository, "
        "a.source_revision, a.source_path, a.image, a.framework, a.port, a.replicas, a.host, "
        "a.auth, a.bindings_json::text, a.env_json::text, a.secret_env_refs::text, a.desired, "
        "a.generation, a.phase, a.url, a.ready, a.message, a.observed_generation, "
        "to_char(a.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "a.source_entrypoint, a.token_hashes_json::text, COALESCE(a.workspace_id::text, '')";

    static WebApp web_app_from(const pqxx::row& row) {
        WebApp app;
        app.id = row[0].as<std::string>();
        app.customer_id = row[1].as<std::string>();
        app.name = row[2].as<std::string>();
        app.mode = row[3].as<std::string>();
        app.source_repository = row[4].as<std::string>();
        app.source_revision = row[5].as<std::string>();
        app.source_path = row[6].as<std::string>();
        app.image = row[7].as<std::string>();
        app.framework = row[8].as<std::string>();
        app.port = row[9].as<int>();
        app.replicas = row[10].as<int>();
        app.host = row[11].as<std::string>();
        app.auth = row[12].as<std::string>();
        app.bindings = nlohmann::json::parse(row[13].as<std::string>());
        app.env = nlohmann::json::parse(row[14].as<std::string>());
        app.secret_env_refs = nlohmann::json::parse(row[15].as<std::string>());
        app.desired = row[16].as<std::string>();
        app.generation = row[17].as<std::int64_t>();
        app.phase = row[18].as<std::string>();
        app.url = row[19].as<std::string>();
        app.ready = row[20].as<bool>();
        app.message = row[21].as<std::string>();
        app.observed_generation = row[22].as<std::int64_t>();
        app.updated_at = row[23].as<std::string>();
        app.source_entrypoint = row[24].as<std::string>();
        app.token_hashes = nlohmann::json::parse(row[25].as<std::string>());
        app.workspace_id = row[26].as<std::string>();
        return app;
    }

    static Result<WebApp> one_web_app(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + web_app_columns + " FROM apps a WHERE a.id = $1::uuid", id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no app " + id});
        }
        return web_app_from(rows[0]);
    }

    Result<std::vector<WebApp>> list_web_apps(const std::string& customer_id) override {
        return with_tx<std::vector<WebApp>>([&](pqxx::work& tx) -> Result<std::vector<WebApp>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + web_app_columns +
                                   " FROM apps a WHERE a.customer_id = $1::uuid "
                                   "AND NOT (a.desired = 'deleted' AND a.phase = 'Deleted') "
                                   "ORDER BY a.updated_at, a.id",
                               customer_id);
            std::vector<WebApp> apps;
            for (const pqxx::row& row : rows) {
                apps.push_back(web_app_from(row));
            }
            return apps;
        });
    }

    Result<WebApp> get_web_app(const std::string& id) override {
        return with_tx<WebApp>(
            [&](pqxx::work& tx) -> Result<WebApp> { return one_web_app(tx, id); });
    }

    Result<WebApp> resolve_web_app_auth_host(const std::string& host,
                                             const std::string& workspace_id) override {
        return with_tx<WebApp>([&](pqxx::work& tx) -> Result<WebApp> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + web_app_columns +
                    " FROM apps a JOIN workspaces w ON w.id = a.workspace_id "
                    "WHERE a.host = $1 AND a.auth = 'lake' AND a.desired <> 'deleted' "
                    "AND w.desired <> 'deleted' "
                    "AND (NULLIF($2, '') IS NULL OR a.workspace_id = $2::uuid)",
                host,
                workspace_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no protected app uses this host"});
            }
            return web_app_from(rows[0]);
        });
    }

    Result<WebApp> authorize_web_app(const std::string& app_id,
                                     const std::string& user_id) override {
        return with_tx<WebApp>([&](pqxx::work& tx) -> Result<WebApp> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + web_app_columns +
                    ", (c.customer_type = 'individual' AND c.user_id = $2::uuid) OR "
                    "(c.customer_type = 'organization' AND EXISTS ("
                    "SELECT 1 FROM memberships m WHERE m.organization_id = c.organization_id "
                    "AND m.principal_type = 'user' AND m.principal_id = $2::text)) "
                    "FROM apps a JOIN workspaces w ON w.id = a.workspace_id "
                    "JOIN customers c ON c.id = a.customer_id "
                    "WHERE a.id = $1::uuid AND a.auth = 'lake' "
                    "AND a.desired <> 'deleted' AND w.desired <> 'deleted'",
                app_id,
                user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no protected app " + app_id});
            }
            if (!rows[0][27].as<bool>()) {
                return std::unexpected(
                    StoreError{"unauthorized", "user is not a member of this account"});
            }
            return web_app_from(rows[0]);
        });
    }

    Result<WebApp> authorize_web_app_session(const std::string& host,
                                             const std::string& workspace_id,
                                             const std::string& app_id,
                                             const std::string& user_id) override {
        return with_tx<WebApp>([&](pqxx::work& tx) -> Result<WebApp> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + web_app_columns +
                    " FROM apps a JOIN workspaces w ON w.id = a.workspace_id "
                    "JOIN customers c ON c.id = a.customer_id "
                    "WHERE a.host = $1 AND a.workspace_id = $2::uuid AND a.id = $3::uuid "
                    "AND a.auth = 'lake' AND a.desired <> 'deleted' AND w.desired <> 'deleted' "
                    "AND ((c.customer_type = 'individual' AND c.user_id = $4::uuid) OR "
                    "(c.customer_type = 'organization' AND EXISTS ("
                    "SELECT 1 FROM memberships m WHERE m.organization_id = c.organization_id "
                    "AND m.principal_type = 'user' AND m.principal_id = $4::text)))",
                host,
                workspace_id,
                app_id,
                user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"unauthorized", "app session is not authorized"});
            }
            return web_app_from(rows[0]);
        });
    }

    Result<void> create_web_app_authorization_code(const WebAppAuthorizationCode& value) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("INSERT INTO app_authorization_codes (code_hash, app_id, user_id, host) "
                           "VALUES ($1, $2::uuid, $3::uuid, $4)",
                           value.code_hash,
                           value.app_id,
                           value.user_id,
                           value.host);
            return {};
        });
    }

    Result<WebAppAuthorization>
    consume_web_app_authorization_code(const std::string& host,
                                       const std::string& workspace_id,
                                       const std::string& code_hash) override {
        return with_tx<WebAppAuthorization>([&](pqxx::work& tx) -> Result<WebAppAuthorization> {
            const pqxx::result rows = tx.exec_params(
                std::string("WITH spent AS (") +
                    "UPDATE app_authorization_codes code SET used_at = now() "
                    "FROM apps candidate JOIN workspaces w "
                    "ON w.id = candidate.workspace_id JOIN customers c "
                    "ON c.id = candidate.customer_id "
                    "WHERE code.code_hash = $1 AND code.app_id = candidate.id "
                    "AND code.host = $2 AND code.expires_at > now() "
                    "AND code.used_at IS NULL AND candidate.host = $2 "
                    "AND candidate.auth = 'lake' AND candidate.desired <> 'deleted' "
                    "AND w.desired <> 'deleted' "
                    "AND (NULLIF($3, '') IS NULL OR candidate.workspace_id = $3::uuid) "
                    "AND ((c.customer_type = 'individual' AND c.user_id = code.user_id) OR "
                    "(c.customer_type = 'organization' AND EXISTS ("
                    "SELECT 1 FROM memberships m WHERE m.organization_id = c.organization_id "
                    "AND m.principal_type = 'user' "
                    "AND m.principal_id = code.user_id::text))) "
                    "RETURNING code.app_id, code.user_id) SELECT " +
                    web_app_columns +
                    ", spent.user_id::text FROM spent JOIN apps a ON a.id = spent.app_id",
                code_hash,
                host,
                workspace_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "app authorization code is invalid or expired"});
            }
            return WebAppAuthorization{web_app_from(rows[0]), rows[0][27].as<std::string>()};
        });
    }

    Result<void> sweep_web_app_authorization_codes() override {
        return with_tx<void>([](pqxx::work& tx) -> Result<void> {
            tx.exec("DELETE FROM app_authorization_codes "
                    "WHERE expires_at <= now() OR used_at IS NOT NULL");
            return {};
        });
    }

    Result<WebApp> create_web_app(const NewWebApp& input) override {
        const auto workspace = get_workspace(input.workspace_id);
        if (!workspace || workspace->customer_id != input.customer_id ||
            workspace->desired == "deleted") {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
        }
        return with_tx<WebApp>([&](pqxx::work& tx) -> Result<WebApp> {
            const pqxx::result customers = tx.exec_params(
                "SELECT 1 FROM customers WHERE id = $1::uuid FOR UPDATE", input.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
            }
            const pqxx::result conflicts = tx.exec_params(
                "SELECT customer_id::text, name, host FROM apps WHERE desired <> 'deleted' "
                "AND ((customer_id = $1::uuid AND name = $2) OR host = $3)",
                input.customer_id,
                input.name,
                input.host);
            if (!conflicts.empty()) {
                const bool same_name = conflicts[0][0].as<std::string>() == input.customer_id &&
                                       conflicts[0][1].as<std::string>() == input.name;
                return std::unexpected(
                    StoreError{"conflict",
                               same_name ? "an app named " + input.name + " already exists"
                                         : "an app already uses host " + input.host});
            }
            const pqxx::result usage =
                tx.exec_params("SELECT count(*), COALESCE(sum(replicas), 0) FROM apps "
                               "WHERE customer_id = $1::uuid AND desired <> 'deleted'",
                               input.customer_id);
            if (input.max_apps != 0 && usage[0][0].as<std::size_t>() >= input.max_apps) {
                return std::unexpected(StoreError{"quota_exceeded", "app quota reached"});
            }
            if (input.max_replicas != 0 &&
                usage[0][1].as<std::size_t>() + static_cast<std::size_t>(input.replicas) >
                    input.max_replicas) {
                return std::unexpected(StoreError{"quota_exceeded", "app replica quota reached"});
            }
            const std::string id = new_uuid_v4();
            nlohmann::json bindings = input.bindings;
            tx.exec_params(
                "INSERT INTO apps (id, customer_id, name, mode, source_repository, "
                "source_revision, source_path, image, framework, port, replicas, host, auth, "
                "bindings_json, env_json, secret_env_refs, source_entrypoint, workspace_id) VALUES ($1::uuid, "
                "$2::uuid, $3, $4, "
                "$5, $6, $7, $8, $9, $10, $11, $12, $13, $14::jsonb, $15::jsonb, $16::jsonb, $17, $18::uuid)",
                id,
                input.customer_id,
                input.name,
                input.mode,
                input.source_repository,
                input.source_revision,
                input.source_path,
                input.image,
                input.framework,
                input.port,
                input.replicas,
                input.host,
                input.auth,
                bindings.dump(),
                input.env.dump(),
                input.secret_env_refs.dump(),
                input.source_entrypoint,
                input.workspace_id);
            if (!input.stores.empty()) {
                if (auto written =
                        write_store_mounts(tx, "app", id, input.workspace_id, input.stores);
                    !written) {
                    return std::unexpected(written.error());
                }
                tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            }
            tx.exec_params("SELECT pg_notify('lake_apps', $1)", id);
            return one_web_app(tx, id);
        });
    }

    Result<WebApp> update_web_app(const std::string& id, const WebAppPatch& patch) override {
        return with_tx<WebApp>([&](pqxx::work& tx) -> Result<WebApp> {
            const pqxx::result locked = tx.exec_params(
                "SELECT customer_id::text FROM apps WHERE id = $1::uuid FOR UPDATE", id);
            if (locked.empty()) {
                return std::unexpected(StoreError{"not_found", "no app " + id});
            }
            const std::string customer_id = locked[0][0].as<std::string>();
            tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid FOR UPDATE", customer_id);
            Result<WebApp> current = one_web_app(tx, id);
            if (!current) {
                return current;
            }
            if (current->desired == "deleted") {
                return std::unexpected(StoreError{"conflict", "the app is already deleted"});
            }
            const int replicas = patch.replicas.value_or(current->replicas);
            const std::string desired = patch.desired.value_or(current->desired);
            if (patch.max_replicas != 0 && desired != "deleted") {
                const pqxx::result usage = tx.exec_params(
                    "SELECT COALESCE(sum(replicas), 0) FROM apps WHERE customer_id = $1::uuid "
                    "AND id <> $2::uuid AND desired <> 'deleted'",
                    customer_id,
                    id);
                if (usage[0][0].as<std::size_t>() + static_cast<std::size_t>(replicas) >
                    patch.max_replicas) {
                    return std::unexpected(
                        StoreError{"quota_exceeded", "app replica quota reached"});
                }
            }
            current->mode = patch.mode.value_or(current->mode);
            current->source_repository =
                patch.source_repository.value_or(current->source_repository);
            current->source_revision = patch.source_revision.value_or(current->source_revision);
            current->source_path = patch.source_path.value_or(current->source_path);
            current->source_entrypoint =
                patch.source_entrypoint.value_or(current->source_entrypoint);
            current->image = patch.image.value_or(current->image);
            current->replicas = replicas;
            current->auth = patch.auth.value_or(current->auth);
            current->bindings = patch.bindings.value_or(current->bindings);
            current->env = patch.env.value_or(current->env);
            current->secret_env_refs = patch.secret_env_refs.value_or(current->secret_env_refs);
            current->desired = desired;
            tx.exec_params(
                "UPDATE apps SET mode = $2, source_repository = $3, source_revision = $4, "
                "source_path = $5, image = $6, replicas = $7, bindings_json = $8::jsonb, "
                "env_json = $9::jsonb, secret_env_refs = $10::jsonb, desired = $11, "
                "source_entrypoint = $12, auth = $13, generation = generation + 1, "
                "updated_at = now() "
                "WHERE id = $1::uuid",
                id,
                current->mode,
                current->source_repository,
                current->source_revision,
                current->source_path,
                current->image,
                current->replicas,
                current->bindings.dump(),
                current->env.dump(),
                current->secret_env_refs.dump(),
                current->desired,
                current->source_entrypoint,
                current->auth);
            if (patch.rotate_tokens) {
                tx.exec_params(
                    "UPDATE apps SET token_hashes_json = '{}'::jsonb WHERE id = $1::uuid", id);
            }
            // The stores it mounts (lake#666): replaced when asked, and none once deleted.
            if (desired == "deleted") {
                tx.exec_params(
                    "DELETE FROM store_mounts WHERE object_kind = 'app' AND object_id = $1::uuid",
                    id);
                tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            } else if (patch.stores) {
                if (auto written =
                        write_store_mounts(tx, "app", id, current->workspace_id, *patch.stores);
                    !written) {
                    return std::unexpected(written.error());
                }
                tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            }
            tx.exec_params("SELECT pg_notify('lake_apps', $1)", id);
            return one_web_app(tx, id);
        });
    }

    Result<void> purge_deleted_web_apps() override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec("DELETE FROM store_mounts m USING apps a WHERE m.object_kind = 'app' AND "
                    "a.id = m.object_id AND a.desired = 'deleted' AND a.phase = 'Deleted'");
            tx.exec("DELETE FROM apps WHERE desired = 'deleted' AND phase = 'Deleted'");
            return {};
        });
    }

    static constexpr const char* build_columns =
        "b.id::text, b.customer_id::text, b.repository, b.revision, b.context, b.dockerfile, "
        "b.target, b.build_args_json::text, b.desired, b.phase, b.digest, b.size, b.message, "
        "b.token_hashes_json::text, "
        "to_char(b.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(b.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(b.workspace_id::text, '')";

    static Build build_from(const pqxx::row& row) {
        Build build;
        build.id = row[0].as<std::string>();
        build.customer_id = row[1].as<std::string>();
        build.repository = row[2].as<std::string>();
        build.revision = row[3].as<std::string>();
        build.context = row[4].as<std::string>();
        build.dockerfile = row[5].as<std::string>();
        build.target = row[6].as<std::string>();
        build.build_args = nlohmann::json::parse(row[7].as<std::string>());
        build.desired = row[8].as<std::string>();
        build.phase = row[9].as<std::string>();
        build.digest = row[10].as<std::string>();
        build.size = row[11].as<std::int64_t>();
        build.message = row[12].as<std::string>();
        build.token_hashes = nlohmann::json::parse(row[13].as<std::string>());
        build.created_at = row[14].as<std::string>();
        build.updated_at = row[15].as<std::string>();
        build.workspace_id = row[16].as<std::string>();
        return build;
    }

    static Result<Build> one_build(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + build_columns + " FROM builds b WHERE b.id = $1::uuid", id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no build " + id});
        }
        return build_from(rows[0]);
    }

    Result<std::vector<Build>> list_builds(const std::string& customer_id) override {
        return with_tx<std::vector<Build>>([&](pqxx::work& tx) -> Result<std::vector<Build>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + build_columns +
                                   " FROM builds b WHERE b.customer_id = $1::uuid ORDER BY "
                                   "b.created_at DESC, b.id",
                               customer_id);
            std::vector<Build> builds;
            for (const pqxx::row& row : rows) {
                builds.push_back(build_from(row));
            }
            return builds;
        });
    }

    Result<Build> get_build(const std::string& id) override {
        return with_tx<Build>([&](pqxx::work& tx) -> Result<Build> { return one_build(tx, id); });
    }

    Result<Build> create_build(const NewBuild& input) override {
        const auto workspace = get_workspace(input.workspace_id);
        if (!workspace || workspace->customer_id != input.customer_id ||
            workspace->desired == "deleted") {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
        }
        return with_tx<Build>([&](pqxx::work& tx) -> Result<Build> {
            const pqxx::result repositories = tx.exec_params(
                "SELECT 1 FROM repositories WHERE id = $1::uuid AND customer_id = $2::uuid "
                "AND deleted_at IS NULL",
                input.repository,
                input.customer_id);
            if (repositories.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no such repository in this customer"});
            }
            const std::string id = new_uuid_v4();
            tx.exec_params(
                "INSERT INTO builds (id, customer_id, repository, revision, context, dockerfile, "
                "target, build_args_json, workspace_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, "
                "$8::jsonb, $9::uuid)",
                id,
                input.customer_id,
                input.repository,
                input.revision,
                input.context,
                input.dockerfile,
                input.target,
                input.build_args.dump(),
                input.workspace_id);
            tx.exec_params("SELECT pg_notify('lake_builds', $1)", id);
            return one_build(tx, id);
        });
    }

    Result<Build> cancel_build(const std::string& id) override {
        return with_tx<Build>([&](pqxx::work& tx) -> Result<Build> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE builds SET desired = 'cancelled', updated_at = now() WHERE id = $1::uuid "
                "AND desired = 'queued' AND phase NOT IN ('Succeeded', 'Failed', 'Cancelled') "
                "RETURNING id",
                id);
            if (rows.empty()) {
                const Result<Build> existing = one_build(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(
                    existing->desired == "cancelled" || existing->phase == "Cancelled"
                        ? StoreError{"conflict", "the build is already cancelled"}
                        : StoreError{"conflict", "the build has already finished"});
            }
            tx.exec_params("SELECT pg_notify('lake_builds', $1)", id);
            return one_build(tx, id);
        });
    }

    static constexpr const char* store_columns =
        "s.id::text, s.customer_id::text, s.type, s.provider, s.name, s.slug, "
        "COALESCE((s.spec->>'size_gi')::int, CASE WHEN s.type = 'volume' THEN 10 ELSE 0 END), "
        "COALESCE(s.spec->>'region', ''), "
        "COALESCE((s.spec->>'public')::boolean, false), "
        "s.desired, s.generation, s.created_by_taap_id, "
        "to_char(s.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(s.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(st.phase, ''), COALESCE(st.message, ''), COALESCE(st.endpoint, ''), "
        "COALESCE(st.observed_generation, 0), COALESCE(s.workspace_id::text, ''), "
        "COALESCE(s.spec->>'bucket', ''), COALESCE(s.spec->>'endpoint', ''), "
        "COALESCE(s.spec->>'prefix', ''), COALESCE(s.spec->'credential'->>'mount_id', ''), "
        "COALESCE(s.spec->'credential'->>'name', ''), COALESCE(s.spec->>'repository_id', ''), "
        "st.used_bytes, COALESCE(to_char(st.used_bytes_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')";

    static ManagedStore store_from(const pqxx::row& row) {
        ManagedStore store;
        store.id = row[0].as<std::string>();
        store.customer_id = row[1].as<std::string>();
        store.type = row[2].as<std::string>();
        store.provider = row[3].as<std::string>();
        store.name = row[4].as<std::string>();
        store.slug = row[5].as<std::string>();
        store.size_gi = row[6].as<int>();
        store.region = row[7].as<std::string>();
        store.is_public = row[8].as<bool>();
        store.desired = row[9].as<std::string>();
        store.generation = row[10].as<std::int64_t>();
        store.created_by_taap_id = row[11].as<std::string>();
        store.created_at = row[12].as<std::string>();
        store.updated_at = row[13].as<std::string>();
        store.phase = row[14].as<std::string>();
        store.message = row[15].as<std::string>();
        store.endpoint = row[16].as<std::string>();
        store.observed_generation = row[17].as<std::int64_t>();
        store.workspace_id = row[18].as<std::string>();
        store.bucket = row[19].as<std::string>();
        store.endpoint_url = row[20].as<std::string>();
        store.prefix = row[21].as<std::string>();
        store.credential_mount_id = row[22].as<std::string>();
        store.credential_name = row[23].as<std::string>();
        store.repository_id = row[24].as<std::string>();
        if (!row[25].is_null()) {
            store.used_bytes = row[25].as<std::int64_t>();
        }
        store.used_bytes_at = row[26].as<std::string>();
        return store;
    }

    static Result<ManagedStore> one_store(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows =
            tx.exec_params(std::string("SELECT ") + store_columns +
                               " FROM stores s LEFT JOIN store_status st ON st.store_id = s.id "
                               "WHERE s.id = $1::uuid",
                           id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no store " + id});
        }
        ManagedStore store = store_from(rows[0]);
        store.mounted_by = store_mounts_of(tx, "", "", id);
        return store;
    }

    Result<std::vector<ManagedStore>> list_stores(const std::string& customer_id,
                                                  const std::string& workspace_id) override {
        return with_tx<std::vector<ManagedStore>>(
            [&](pqxx::work& tx) -> Result<std::vector<ManagedStore>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + store_columns +
                        " FROM stores s LEFT JOIN store_status st ON st.store_id = s.id "
                        "WHERE s.customer_id = $1::uuid AND ($2 = '' OR s.workspace_id = "
                        "NULLIF($2, '')::uuid) AND NOT (s.desired = 'absent' AND "
                        "COALESCE(st.phase, '') = 'Deleted') ORDER BY s.created_at, s.id",
                    customer_id,
                    workspace_id);
                std::vector<ManagedStore> stores;
                for (const pqxx::row& row : rows) {
                    stores.push_back(store_from(row));
                }
                for (StoreMount& mount : store_mounts_of(tx, customer_id, workspace_id, "")) {
                    const auto owner = std::ranges::find(stores, mount.store_id, &ManagedStore::id);
                    if (owner != stores.end()) {
                        owner->mounted_by.push_back(std::move(mount));
                    }
                }
                return stores;
            });
    }

    Result<ManagedStore> get_store(const std::string& id) override {
        return with_tx<ManagedStore>(
            [&](pqxx::work& tx) -> Result<ManagedStore> { return one_store(tx, id); });
    }

    Result<ManagedStore> create_store(const NewManagedStore& in) override {
        return with_tx<ManagedStore>([&](pqxx::work& tx) -> Result<ManagedStore> {
            // One writer at a time per customer, so the name check holds.
            const pqxx::result customers = tx.exec_params(
                "SELECT 1 FROM customers WHERE id = $1::uuid FOR UPDATE", in.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
            }
            const pqxx::result named = tx.exec_params(
                "SELECT 1 FROM stores WHERE customer_id = $1::uuid AND name = $2 AND desired = "
                "'present'",
                in.customer_id,
                in.name);
            if (!named.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "a store named " + in.name + " already exists"});
            }
            const std::string id = new_uuid_v4();
            const nlohmann::json spec = store_spec(in);
            // The routing trigger refuses a store without a live workspace of this space.
            tx.exec_params("INSERT INTO stores (id, customer_id, type, provider, name, slug, spec, "
                           "created_by_taap_id, workspace_id) VALUES ($1::uuid, $2::uuid, $3, $4, "
                           "$5, $6, $7::jsonb, $8, NULLIF($9, '')::uuid)",
                           id,
                           in.customer_id,
                           in.type,
                           in.provider,
                           in.name,
                           managed_service_slug("st", id),
                           spec.dump(),
                           in.created_by_taap_id,
                           in.workspace_id);
            // Same channel as services: the operator's listener wakes the Lake at once;
            // delivered on commit.
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_store(tx, id);
        });
    }

    Result<ManagedStore> withdraw_store(const std::string& id) override {
        return with_tx<ManagedStore>([&](pqxx::work& tx) -> Result<ManagedStore> {
            // Locked first, as write_store_mounts does, so no mount lands in between (lake#666).
            tx.exec_params("SELECT 1 FROM stores WHERE id = $1::uuid FOR UPDATE", id);
            const std::vector<StoreMount> mounts = store_mounts_of(tx, "", "", id);
            if (!mounts.empty()) {
                return std::unexpected(StoreError{"conflict", mounted_by_message(mounts)});
            }
            const pqxx::result rows = tx.exec_params(
                "UPDATE stores SET desired = 'absent', generation = generation + 1, "
                "updated_at = now() WHERE id = $1::uuid AND desired = 'present' RETURNING id",
                id);
            if (rows.empty()) {
                const Result<ManagedStore> existing = one_store(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(StoreError{"conflict", "the store is already deleted"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_store(tx, id);
        });
    }

    static constexpr const char* launch_columns =
        "l.id::text, l.customer_id::text, l.agent_id, l.agent_name, l.mode, l.slug, l.desired, "
        "l.trigger, l.input::text, array_to_json(l.scopes)::text, l.generation, "
        "COALESCE(l.created_by_user_id::text, ''), l.created_by_taap_id, "
        "to_char(l.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(l.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(ls.phase, ''), COALESCE(ls.message, ''), COALESCE(ls.endpoint, ''), "
        "ls.exit_code, COALESCE(ls.result, 'null'::jsonb)::text, "
        "COALESCE(ls.credential_sha256, ''), COALESCE(ls.observed_generation, 0), "
        "COALESCE(l.workspace_id::text, ''), l.stop_reason, "
        "COALESCE(to_char(ls.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "'')";

    static AgentLaunch launch_from(const pqxx::row& row) {
        AgentLaunch launch;
        launch.id = row[0].as<std::string>();
        launch.customer_id = row[1].as<std::string>();
        launch.agent_id = row[2].as<std::string>();
        launch.agent_name = row[3].as<std::string>();
        launch.mode = row[4].as<std::string>();
        launch.slug = row[5].as<std::string>();
        launch.desired = row[6].as<std::string>();
        launch.trigger = row[7].as<std::string>();
        launch.input = nlohmann::json::parse(row[8].as<std::string>(), nullptr, false);
        launch.scopes =
            nlohmann::json::parse(row[9].as<std::string>()).get<std::vector<std::string>>();
        launch.generation = row[10].as<std::int64_t>();
        launch.created_by_user_id = row[11].as<std::string>();
        launch.created_by_taap_id = row[12].as<std::string>();
        launch.created_at = row[13].as<std::string>();
        launch.updated_at = row[14].as<std::string>();
        launch.phase = row[15].as<std::string>();
        launch.message = row[16].as<std::string>();
        launch.endpoint = row[17].as<std::string>();
        if (!row[18].is_null()) {
            launch.exit_code = row[18].as<int>();
        }
        launch.result = nlohmann::json::parse(row[19].as<std::string>(), nullptr, false);
        launch.credential_sha256 = row[20].as<std::string>();
        launch.observed_generation = row[21].as<std::int64_t>();
        launch.workspace_id = row[22].as<std::string>();
        launch.stop_reason = row[23].as<std::string>();
        launch.status_updated_at = row[24].as<std::string>();
        return launch;
    }

    static Result<AgentLaunch> one_launch(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + launch_columns +
                " FROM agent_launches l LEFT JOIN agent_launch_status ls ON ls.launch_id = l.id "
                "WHERE l.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no agent launch " + id});
        }
        return launch_from(rows[0]);
    }

    // Lock the person before changing their delegated work. Offboarding locks the same row
    // first, so a concurrent create/claim/renew either precedes revocation or is refused.
    static Result<void>
    active_delegator(pqxx::work& tx, const std::string& user_id, bool exclusive = false) {
        if (user_id.empty()) {
            return {}; // system work carries no person's authority
        }
        const auto user =
            tx.exec_params(std::string("SELECT ") + user_columns +
                               " FROM users WHERE id = $1::uuid AND deleted_at IS NULL" +
                               (exclusive ? " FOR UPDATE" : " FOR SHARE"),
                           user_id);
        if (user.empty() || !user_of(user[0]).may_act()) {
            return std::unexpected(StoreError{"unauthorized", "delegating account is not active"});
        }
        return {};
    }

    static void revoke_agent_delegations(pqxx::work& tx, const std::string& user_id) {
        const auto launches = tx.exec_params(
            "UPDATE agent_launches SET desired = 'absent', generation = generation + 1, "
            "stop_reason = 'Stopped: the account that started this run is disabled.', "
            "updated_at = now() WHERE created_by_user_id = $1::uuid AND desired = 'present' "
            "RETURNING id::text",
            user_id);
        const auto schedules = tx.exec_params(
            "UPDATE agent_cron_schedules SET desired = 'absent', generation = generation + 1, "
            "updated_at = now() WHERE created_by_user_id = $1::uuid AND desired = 'present' "
            "RETURNING id::text",
            user_id);
        tx.exec_params("UPDATE agent_tasks SET desired = 'absent' "
                       "WHERE requested_by_user_id = $1::uuid AND desired = 'present'",
                       user_id);
        tx.exec_params(
            "INSERT INTO agent_task_status AS ts (task_id, phase, notice, finished_at) "
            "SELECT id, 'Failed', 'Stopped: the account that requested this task is disabled.', "
            "now() "
            "FROM agent_tasks WHERE requested_by_user_id = $1::uuid "
            "ON CONFLICT (task_id) DO UPDATE SET credential_sha256 = '', lease_until = NULL, "
            "retry_notice_pending = false, "
            "phase = CASE WHEN ts.phase IN ('', 'Running') THEN 'Failed' ELSE ts.phase END, "
            "notice = CASE WHEN ts.phase IN ('', 'Running') THEN "
            "'Stopped: the account that requested this task is disabled.' ELSE ts.notice END, "
            "finished_at = coalesce(ts.finished_at, now()), updated_at = now()",
            user_id);
        for (const pqxx::result* rows : {&launches, &schedules}) {
            for (const pqxx::row& row : *rows) {
                tx.exec_params("SELECT pg_notify('lake_services', $1)", row[0].as<std::string>());
            }
        }
    }

    // A launch credential (lake#231): the launch must be present, the operator must have written
    // the credential's digest, and the digests must match. It acts as the person who launched.
    Result<Principal> launch_principal(const std::string& secret) {
        const auto parsed = parse_agent_launch_credential(secret);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows = tx.exec_params(
                "SELECT COALESCE(ls.credential_sha256, ''), array_to_json(l.scopes)::text, "
                "COALESCE(users.id::text, ''), COALESCE(users.taap_id, '') FROM agent_launches l "
                "LEFT JOIN agent_launch_status ls ON ls.launch_id = l.id LEFT JOIN users ON "
                "users.id = l.created_by_user_id WHERE l.id = $1::uuid AND l.desired = 'present' "
                "AND users.id IS NOT NULL AND users.disabled_at IS NULL AND users.deleted_at IS "
                "NULL",
                parsed->first);
            if (rows.empty() || !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                                         rows[0][0].as<std::string>())) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            if (rows[0][3].as<std::string>().empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "launch identity is unavailable"});
            }
            return Principal{
                rows[0][2].as<std::string>(),
                rows[0][3].as<std::string>(),
                "launch:" + parsed->first,
                "",
                "lake",
                "agent_launch",
                nlohmann::json::parse(rows[0][1].as<std::string>()).get<std::vector<std::string>>(),
                std::nullopt};
        });
    }

    Result<AgentLaunch> create_agent_launch(const NewAgentLaunch& in) override {
        if ((in.mode != "job" && in.mode != "service") || in.agent_id.empty() ||
            in.scopes.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent launch is invalid"});
        }
        return with_tx<AgentLaunch>([&](pqxx::work& tx) -> Result<AgentLaunch> {
            if (auto active = active_delegator(tx, in.created_by_user_id); !active) {
                return std::unexpected(active.error());
            }
            const pqxx::result customers =
                tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", in.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
            }
            const std::string id = new_uuid_v4();
            tx.exec_params(
                "INSERT INTO agent_launches (id, customer_id, agent_id, agent_name, mode, input, "
                "scopes, slug, trigger, created_by_user_id, created_by_taap_id, workspace_id) "
                "VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6::jsonb, "
                "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, $9, "
                "NULLIF($10, '')::uuid, $11, NULLIF($12, '')::uuid)",
                id,
                in.customer_id,
                in.agent_id,
                in.agent_name,
                in.mode,
                in.input.dump(),
                nlohmann::json(in.scopes).dump(),
                managed_service_slug("al", id),
                in.trigger.empty() ? std::string("manual") : in.trigger,
                in.created_by_user_id,
                in.created_by_taap_id,
                in.workspace_id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_launch(tx, id);
        });
    }

    Result<AgentLaunch> get_agent_launch(const std::string& id) override {
        return with_tx<AgentLaunch>(
            [&](pqxx::work& tx) -> Result<AgentLaunch> { return one_launch(tx, id); });
    }

    Result<std::vector<AgentLaunch>> list_agent_launches(const std::string& customer_id,
                                                         const std::string& agent_id,
                                                         int limit,
                                                         const std::string& workspace_id) override {
        return with_tx<std::vector<AgentLaunch>>(
            [&](pqxx::work& tx) -> Result<std::vector<AgentLaunch>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + launch_columns +
                        " FROM agent_launches l LEFT JOIN agent_launch_status ls ON "
                        "ls.launch_id = l.id WHERE l.customer_id = $1::uuid AND ($2 = '' OR "
                        "l.agent_id = $2) AND ($4 = '' OR l.workspace_id = NULLIF($4, '')::uuid) "
                        "ORDER BY l.created_at DESC, l.id LIMIT $3",
                    customer_id,
                    agent_id,
                    limit > 0 ? limit : 100,
                    workspace_id);
                std::vector<AgentLaunch> launches;
                for (const pqxx::row& row : rows) {
                    launches.push_back(launch_from(row));
                }
                return launches;
            });
    }

    Result<AgentLaunch> withdraw_agent_launch(const std::string& id) override {
        return with_tx<AgentLaunch>([&](pqxx::work& tx) -> Result<AgentLaunch> {
            tx.exec_params("UPDATE agent_launches SET desired = 'absent', generation = "
                           "generation + 1, updated_at = now() WHERE id = $1::uuid AND "
                           "desired = 'present'",
                           id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_launch(tx, id);
        });
    }

    Result<std::size_t> expire_unstarted_agent_launches(std::int64_t seconds,
                                                        const std::string& reason) override {
        return with_tx<std::size_t>([&](pqxx::work& tx) -> Result<std::size_t> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE agent_launches l SET desired = 'absent', generation = generation + 1, "
                "stop_reason = $2, updated_at = now() WHERE l.desired = 'present' AND "
                "l.created_at < now() - make_interval(secs => $1::double precision) AND NOT "
                "EXISTS (SELECT 1 FROM agent_launch_status ls WHERE ls.launch_id = l.id) "
                "RETURNING l.id::text",
                seconds,
                reason);
            for (const pqxx::row& row : rows) {
                tx.exec_params("SELECT pg_notify('lake_services', $1)", row[0].as<std::string>());
            }
            return rows.size();
        });
    }

    Result<AgentWorkloadsWithdrawn> withdraw_agent_workloads(const std::string& customer_id,
                                                             const std::string& workspace_id,
                                                             const std::string& agent_id,
                                                             const std::string& reason) override {
        return with_tx<AgentWorkloadsWithdrawn>(
            [&](pqxx::work& tx) -> Result<AgentWorkloadsWithdrawn> {
                AgentWorkloadsWithdrawn withdrawn;
                const pqxx::result launches = tx.exec_params(
                    "UPDATE agent_launches SET desired = 'absent', generation = generation + 1, "
                    "stop_reason = $4, updated_at = now() WHERE customer_id = $1::uuid AND "
                    "agent_id = $3 AND desired = 'present' AND (workspace_id IS NULL OR "
                    "workspace_id = NULLIF($2, '')::uuid) RETURNING id::text",
                    customer_id,
                    workspace_id,
                    agent_id,
                    reason);
                const pqxx::result schedules = tx.exec_params(
                    "UPDATE agent_cron_schedules SET desired = 'absent', generation = "
                    "generation + 1, updated_at = now() WHERE customer_id = $1::uuid AND "
                    "agent_id = $3 AND desired = 'present' AND (workspace_id IS NULL OR "
                    "workspace_id = NULLIF($2, '')::uuid) RETURNING id::text",
                    customer_id,
                    workspace_id,
                    agent_id);
                for (const pqxx::result* rows : {&launches, &schedules}) {
                    for (const pqxx::row& row : *rows) {
                        tx.exec_params("SELECT pg_notify('lake_services', $1)",
                                       row[0].as<std::string>());
                    }
                }
                withdrawn.launches = launches.size();
                withdrawn.schedules = schedules.size();
                return withdrawn;
            });
    }

    // Methodology runs (lake#655, migration 0078).
    static constexpr const char* methodology_run_columns =
        "r.id::text, r.customer_id::text, r.workspace_id::text, r.methodology_id, r.mode, "
        "COALESCE(r.build_id::text, ''), r.image, r.runtime_image, r.slug, r.desired, "
        "r.cpu_millis, r.memory_bytes, r.gpus, r.gpu_class, r.timeout_seconds, r.network, "
        "r.generation, r.stop_reason, COALESCE(r.created_by_user_id::text, ''), "
        "r.created_by_taap_id, "
        "to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(r.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(rs.phase, ''), COALESCE(rs.message, ''), rs.exit_code, "
        "COALESCE(rs.credential_sha256, ''), COALESCE(rs.observed_generation, 0), "
        "COALESCE(to_char(rs.started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "''), "
        "COALESCE(to_char(rs.finished_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "''), "
        "COALESCE(to_char(rs.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')";

    static MethodologyRun methodology_run_from(const pqxx::row& row) {
        MethodologyRun run;
        run.id = row[0].as<std::string>();
        run.customer_id = row[1].as<std::string>();
        run.workspace_id = row[2].as<std::string>();
        run.methodology_id = row[3].as<std::string>();
        run.mode = row[4].as<std::string>();
        run.build_id = row[5].as<std::string>();
        run.image = row[6].as<std::string>();
        run.runtime_image = row[7].as<std::string>();
        run.slug = row[8].as<std::string>();
        run.desired = row[9].as<std::string>();
        run.cpu_millis = row[10].as<std::int64_t>();
        run.memory_bytes = row[11].as<std::int64_t>();
        run.gpus = row[12].as<int>();
        run.gpu_class = row[13].as<std::string>();
        run.timeout_seconds = row[14].as<int>();
        run.network = row[15].as<bool>();
        run.generation = row[16].as<std::int64_t>();
        run.stop_reason = row[17].as<std::string>();
        run.created_by_user_id = row[18].as<std::string>();
        run.created_by_taap_id = row[19].as<std::string>();
        run.created_at = row[20].as<std::string>();
        run.updated_at = row[21].as<std::string>();
        run.phase = row[22].as<std::string>();
        run.message = row[23].as<std::string>();
        if (!row[24].is_null()) {
            run.exit_code = row[24].as<int>();
        }
        run.credential_sha256 = row[25].as<std::string>();
        run.observed_generation = row[26].as<std::int64_t>();
        run.started_at = row[27].as<std::string>();
        run.finished_at = row[28].as<std::string>();
        run.status_updated_at = row[29].as<std::string>();
        return run;
    }

    static Result<MethodologyRun> one_methodology_run(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + methodology_run_columns +
                " FROM methodology_runs r LEFT JOIN methodology_run_status rs ON rs.run_id = r.id "
                "WHERE r.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no methodology run " + id});
        }
        return methodology_run_from(rows[0]);
    }

    // A methodology run's credential (lake#655): live while the run is present and unfinished,
    // with the digest the operator wrote. It acts as no person.
    Result<Principal> methodology_run_credential(const std::string& secret) {
        const auto parsed = parse_agent_credential(secret, methodology_run_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            auto run = one_methodology_run(tx, parsed->first);
            const std::string digest = identity_crypto::sha256_hex(secret);
            if (!run || !methodology_run_credential_live(*run) ||
                !constant_time_hex_equal(digest, run->credential_sha256)) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            return methodology_run_principal(*run, digest);
        });
    }

    Result<MethodologyRun> create_methodology_run(const NewMethodologyRun& in) override {
        if ((in.mode != "validate" && in.mode != "train") || in.methodology_id.empty() ||
            in.runtime_image.empty() || in.cpu_millis <= 0 || in.memory_bytes <= 0 || in.gpus < 0 ||
            in.timeout_seconds <= 0) {
            return std::unexpected(StoreError{"invalid_input", "methodology run is invalid"});
        }
        return with_tx<MethodologyRun>([&](pqxx::work& tx) -> Result<MethodologyRun> {
            const std::string id = new_uuid_v4();
            try {
                tx.exec_params(
                    "INSERT INTO methodology_runs (id, customer_id, workspace_id, methodology_id, "
                    "mode, build_id, image, runtime_image, cpu_millis, memory_bytes, gpus, "
                    "gpu_class, timeout_seconds, network, slug, created_by_user_id, "
                    "created_by_taap_id) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, "
                    "NULLIF($6, '')::uuid, $7, $8, $9, $10, $11, $12, $13, $14, $15, "
                    "NULLIF($16, '')::uuid, $17)",
                    id,
                    in.customer_id,
                    in.workspace_id,
                    in.methodology_id,
                    in.mode,
                    in.build_id,
                    in.image,
                    in.runtime_image,
                    in.cpu_millis,
                    in.memory_bytes,
                    in.gpus,
                    in.gpu_class,
                    in.timeout_seconds,
                    in.network,
                    managed_service_slug("mr", id),
                    in.created_by_user_id,
                    in.created_by_taap_id);
            } catch (const pqxx::check_violation&) {
                return std::unexpected(StoreError{
                    "invalid_input", "workspace_id must name a workspace in this space"});
            } catch (const pqxx::foreign_key_violation&) {
                return std::unexpected(StoreError{"not_found", "no such customer or build"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_methodology_run(tx, id);
        });
    }

    Result<MethodologyRun> get_methodology_run(const std::string& id) override {
        return with_tx<MethodologyRun>(
            [&](pqxx::work& tx) -> Result<MethodologyRun> { return one_methodology_run(tx, id); });
    }

    Result<std::vector<MethodologyRun>> list_methodology_runs(const std::string& customer_id,
                                                              const std::string& workspace_id,
                                                              const std::string& methodology_id,
                                                              int limit) override {
        return with_tx<std::vector<MethodologyRun>>(
            [&](pqxx::work& tx) -> Result<std::vector<MethodologyRun>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + methodology_run_columns +
                        " FROM methodology_runs r LEFT JOIN methodology_run_status rs ON "
                        "rs.run_id = r.id WHERE r.customer_id = $1::uuid AND r.workspace_id = "
                        "$2::uuid AND ($3 = '' OR r.methodology_id = $3) ORDER BY r.created_at "
                        "DESC, r.id DESC LIMIT $4",
                    customer_id,
                    workspace_id,
                    methodology_id,
                    limit > 0 ? limit : 100);
                std::vector<MethodologyRun> runs;
                for (const pqxx::row& row : rows) {
                    runs.push_back(methodology_run_from(row));
                }
                return runs;
            });
    }

    Result<MethodologyRun> withdraw_methodology_run(const std::string& id,
                                                    const std::string& reason) override {
        return with_tx<MethodologyRun>([&](pqxx::work& tx) -> Result<MethodologyRun> {
            const pqxx::result changed = tx.exec_params(
                "UPDATE methodology_runs SET desired = 'absent', generation = generation + 1, "
                "stop_reason = $2, updated_at = now() WHERE id = $1::uuid AND desired = "
                "'present' RETURNING id",
                id,
                reason);
            if (changed.empty()) {
                auto existing = one_methodology_run(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(StoreError{"conflict", "the run is already stopped"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_methodology_run(tx, id);
        });
    }

    Result<std::size_t> expire_unstarted_methodology_runs(std::int64_t seconds,
                                                          const std::string& reason) override {
        return with_tx<std::size_t>([&](pqxx::work& tx) -> Result<std::size_t> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE methodology_runs r SET desired = 'absent', generation = generation + 1, "
                "stop_reason = $2, updated_at = now() WHERE r.desired = 'present' AND "
                "r.created_at < now() - make_interval(secs => $1::double precision) AND NOT "
                "EXISTS (SELECT 1 FROM methodology_run_status rs WHERE rs.run_id = r.id AND "
                "rs.phase <> '') RETURNING r.id::text",
                seconds,
                reason);
            for (const pqxx::row& row : rows) {
                tx.exec_params("SELECT pg_notify('lake_services', $1)", row[0].as<std::string>());
            }
            return rows.size();
        });
    }

    Result<void> put_methodology_run_status(const std::string& id,
                                            const MethodologyRunStatus& status) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO methodology_run_status (run_id, phase, message, exit_code, "
                "credential_sha256, observed_generation, started_at, finished_at, updated_at) "
                "SELECT r.id, $2, $3, $4, $5, CASE WHEN $6 = 0 THEN r.generation ELSE $6 END, "
                "NULLIF($7, '')::timestamptz, NULLIF($8, '')::timestamptz, now() FROM "
                "methodology_runs r WHERE r.id = $1::uuid ON CONFLICT (run_id) DO UPDATE SET "
                "phase = EXCLUDED.phase, message = EXCLUDED.message, exit_code = "
                "EXCLUDED.exit_code, credential_sha256 = EXCLUDED.credential_sha256, "
                "observed_generation = EXCLUDED.observed_generation, started_at = "
                "EXCLUDED.started_at, finished_at = EXCLUDED.finished_at, updated_at = now() "
                "RETURNING run_id",
                id,
                status.phase,
                status.message,
                status.exit_code,
                status.credential_sha256,
                status.observed_generation,
                status.started_at,
                status.finished_at);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no methodology run " + id});
            }
            return {};
        });
    }

    // Methodology deployments (lake#690, migration 0083).
    static constexpr const char* methodology_deployment_columns =
        "d.id::text, d.customer_id::text, d.workspace_id::text, d.methodology_id, d.model_id, "
        "COALESCE(d.run_id::text, ''), COALESCE(d.build_id::text, ''), d.image, d.runtime_image, "
        "d.slug, d.desired, d.cpu_millis, d.memory_bytes, d.gpus, d.gpu_class, d.network, "
        "d.generation, d.stop_reason, COALESCE(d.created_by_user_id::text, ''), "
        "d.created_by_taap_id, "
        "to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(ds.phase, ''), COALESCE(ds.message, ''), COALESCE(ds.credential_sha256, ''), "
        "COALESCE(ds.endpoint, ''), COALESCE(ds.observed_generation, 0), "
        "COALESCE(to_char(ds.ready_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(ds.stopped_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "''), "
        "COALESCE(to_char(ds.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')";

    static MethodologyDeployment methodology_deployment_from(const pqxx::row& row) {
        MethodologyDeployment deployment;
        deployment.id = row[0].as<std::string>();
        deployment.customer_id = row[1].as<std::string>();
        deployment.workspace_id = row[2].as<std::string>();
        deployment.methodology_id = row[3].as<std::string>();
        deployment.model_id = row[4].as<std::string>();
        deployment.run_id = row[5].as<std::string>();
        deployment.build_id = row[6].as<std::string>();
        deployment.image = row[7].as<std::string>();
        deployment.runtime_image = row[8].as<std::string>();
        deployment.slug = row[9].as<std::string>();
        deployment.desired = row[10].as<std::string>();
        deployment.cpu_millis = row[11].as<std::int64_t>();
        deployment.memory_bytes = row[12].as<std::int64_t>();
        deployment.gpus = row[13].as<int>();
        deployment.gpu_class = row[14].as<std::string>();
        deployment.network = row[15].as<bool>();
        deployment.generation = row[16].as<std::int64_t>();
        deployment.stop_reason = row[17].as<std::string>();
        deployment.created_by_user_id = row[18].as<std::string>();
        deployment.created_by_taap_id = row[19].as<std::string>();
        deployment.created_at = row[20].as<std::string>();
        deployment.updated_at = row[21].as<std::string>();
        deployment.phase = row[22].as<std::string>();
        deployment.message = row[23].as<std::string>();
        deployment.credential_sha256 = row[24].as<std::string>();
        deployment.endpoint = row[25].as<std::string>();
        deployment.observed_generation = row[26].as<std::int64_t>();
        deployment.ready_at = row[27].as<std::string>();
        deployment.stopped_at = row[28].as<std::string>();
        deployment.status_updated_at = row[29].as<std::string>();
        return deployment;
    }

    static Result<MethodologyDeployment> one_methodology_deployment(pqxx::work& tx,
                                                                    const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + methodology_deployment_columns +
                " FROM methodology_deployments d LEFT JOIN methodology_deployment_status ds ON "
                "ds.deployment_id = d.id WHERE d.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no methodology deployment " + id});
        }
        return methodology_deployment_from(rows[0]);
    }

    // A deployment's credential (lake#690): live while the deployment is present, with the digest
    // the operator wrote. It acts as no person.
    Result<Principal> methodology_deployment_credential(const std::string& secret) {
        const auto parsed = parse_agent_credential(secret, methodology_deployment_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            auto deployment = one_methodology_deployment(tx, parsed->first);
            const std::string digest = identity_crypto::sha256_hex(secret);
            if (!deployment || !methodology_deployment_credential_live(*deployment) ||
                !constant_time_hex_equal(digest, deployment->credential_sha256)) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            return methodology_deployment_principal(*deployment, digest);
        });
    }

    Result<MethodologyDeployment>
    create_methodology_deployment(const NewMethodologyDeployment& in) override {
        if (in.methodology_id.empty() || in.model_id.empty() || in.runtime_image.empty() ||
            in.cpu_millis <= 0 || in.memory_bytes <= 0 || in.gpus < 0) {
            return std::unexpected(StoreError{"invalid_input", "methodology deployment is invalid"});
        }
        return with_tx<MethodologyDeployment>([&](pqxx::work& tx) -> Result<MethodologyDeployment> {
            const std::string id = new_uuid_v4();
            try {
                tx.exec_params(
                    "INSERT INTO methodology_deployments (id, customer_id, workspace_id, "
                    "methodology_id, model_id, run_id, build_id, image, runtime_image, cpu_millis, "
                    "memory_bytes, gpus, gpu_class, network, slug, created_by_user_id, "
                    "created_by_taap_id) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, "
                    "NULLIF($6, '')::uuid, NULLIF($7, '')::uuid, $8, $9, $10, $11, $12, $13, $14, "
                    "$15, NULLIF($16, '')::uuid, $17)",
                    id,
                    in.customer_id,
                    in.workspace_id,
                    in.methodology_id,
                    in.model_id,
                    in.run_id,
                    in.build_id,
                    in.image,
                    in.runtime_image,
                    in.cpu_millis,
                    in.memory_bytes,
                    in.gpus,
                    in.gpu_class,
                    in.network,
                    managed_service_slug("md", id),
                    in.created_by_user_id,
                    in.created_by_taap_id);
            } catch (const pqxx::unique_violation&) {
                return std::unexpected(StoreError{"conflict", "this model is already deployed"});
            } catch (const pqxx::check_violation&) {
                return std::unexpected(StoreError{
                    "invalid_input", "workspace_id must name a workspace in this space"});
            } catch (const pqxx::foreign_key_violation&) {
                return std::unexpected(StoreError{"not_found", "no such customer, run or build"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_methodology_deployment(tx, id);
        });
    }

    Result<MethodologyDeployment> get_methodology_deployment(const std::string& id) override {
        return with_tx<MethodologyDeployment>([&](pqxx::work& tx) -> Result<MethodologyDeployment> {
            return one_methodology_deployment(tx, id);
        });
    }

    Result<std::vector<MethodologyDeployment>>
    list_methodology_deployments(const std::string& customer_id,
                                 const std::string& workspace_id,
                                 const std::string& model_id,
                                 int limit) override {
        return with_tx<std::vector<MethodologyDeployment>>(
            [&](pqxx::work& tx) -> Result<std::vector<MethodologyDeployment>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + methodology_deployment_columns +
                        " FROM methodology_deployments d LEFT JOIN methodology_deployment_status "
                        "ds ON ds.deployment_id = d.id WHERE d.customer_id = $1::uuid AND "
                        "d.workspace_id = $2::uuid AND ($3 = '' OR d.model_id = $3) ORDER BY "
                        "d.created_at DESC, d.id DESC LIMIT $4",
                    customer_id,
                    workspace_id,
                    model_id,
                    limit > 0 ? limit : 100);
                std::vector<MethodologyDeployment> deployments;
                for (const pqxx::row& row : rows) {
                    deployments.push_back(methodology_deployment_from(row));
                }
                return deployments;
            });
    }

    Result<MethodologyDeployment> withdraw_methodology_deployment(const std::string& id,
                                                                  const std::string& reason) override {
        return with_tx<MethodologyDeployment>([&](pqxx::work& tx) -> Result<MethodologyDeployment> {
            const pqxx::result changed = tx.exec_params(
                "UPDATE methodology_deployments SET desired = 'absent', generation = generation + "
                "1, stop_reason = $2, updated_at = now() WHERE id = $1::uuid AND desired = "
                "'present' RETURNING id",
                id,
                reason);
            if (changed.empty()) {
                auto existing = one_methodology_deployment(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(StoreError{"conflict", "the deployment is already stopped"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_methodology_deployment(tx, id);
        });
    }

    Result<void> put_methodology_deployment_status(const std::string& id,
                                                   const MethodologyDeploymentStatus& status) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            pqxx::result rows;
            try {
                rows = tx.exec_params(
                    "INSERT INTO methodology_deployment_status (deployment_id, phase, message, "
                    "credential_sha256, endpoint, observed_generation, ready_at, stopped_at, "
                    "updated_at) SELECT d.id, $2, $3, $4, $5, CASE WHEN $6 = 0 THEN d.generation "
                    "ELSE $6 END, NULLIF($7, '')::timestamptz, NULLIF($8, '')::timestamptz, now() "
                    "FROM methodology_deployments d WHERE d.id = $1::uuid ON CONFLICT "
                    "(deployment_id) DO UPDATE SET phase = EXCLUDED.phase, message = "
                    "EXCLUDED.message, credential_sha256 = EXCLUDED.credential_sha256, endpoint = "
                    "EXCLUDED.endpoint, observed_generation = EXCLUDED.observed_generation, "
                    "ready_at = EXCLUDED.ready_at, stopped_at = EXCLUDED.stopped_at, updated_at = "
                    "now() RETURNING deployment_id",
                    id,
                    status.phase,
                    status.message,
                    status.credential_sha256,
                    status.endpoint,
                    status.observed_generation,
                    status.ready_at,
                    status.stopped_at);
            } catch (const pqxx::check_violation&) {
                return std::unexpected(StoreError{"invalid_input", "not a deployment status"});
            }
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no methodology deployment " + id});
            }
            return {};
        });
    }

    static constexpr const char* cron_schedule_columns =
        "s.id::text, s.customer_id::text, s.agent_id, s.agent_name, s.cron, s.time_zone, "
        "s.description, s.slug, s.desired, s.input::text, array_to_json(s.scopes)::text, "
        "s.generation, COALESCE(s.created_by_user_id::text, ''), s.created_by_taap_id, "
        "to_char(s.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(s.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(ss.phase, ''), COALESCE(ss.message, ''), "
        "COALESCE(to_char(ss.last_schedule_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), COALESCE(ss.credential_sha256, ''), "
        "COALESCE(ss.observed_generation, 0), COALESCE(s.workspace_id::text, '')";

    static AgentCronSchedule cron_schedule_from(const pqxx::row& row) {
        AgentCronSchedule schedule;
        schedule.id = row[0].as<std::string>();
        schedule.customer_id = row[1].as<std::string>();
        schedule.agent_id = row[2].as<std::string>();
        schedule.agent_name = row[3].as<std::string>();
        schedule.cron = row[4].as<std::string>();
        schedule.time_zone = row[5].as<std::string>();
        schedule.description = row[6].as<std::string>();
        schedule.slug = row[7].as<std::string>();
        schedule.desired = row[8].as<std::string>();
        schedule.input = nlohmann::json::parse(row[9].as<std::string>(), nullptr, false);
        schedule.scopes =
            nlohmann::json::parse(row[10].as<std::string>()).get<std::vector<std::string>>();
        schedule.generation = row[11].as<std::int64_t>();
        schedule.created_by_user_id = row[12].as<std::string>();
        schedule.created_by_taap_id = row[13].as<std::string>();
        schedule.created_at = row[14].as<std::string>();
        schedule.updated_at = row[15].as<std::string>();
        schedule.phase = row[16].as<std::string>();
        schedule.message = row[17].as<std::string>();
        schedule.last_schedule_at = row[18].as<std::string>();
        schedule.credential_sha256 = row[19].as<std::string>();
        schedule.observed_generation = row[20].as<std::int64_t>();
        schedule.workspace_id = row[21].as<std::string>();
        return schedule;
    }

    static Result<AgentCronSchedule> one_cron_schedule(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + cron_schedule_columns +
                " FROM agent_cron_schedules s LEFT JOIN agent_cron_schedule_status ss ON "
                "ss.schedule_id = s.id WHERE s.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no agent schedule " + id});
        }
        return cron_schedule_from(rows[0]);
    }

    // A schedule credential: as a launch credential, against the schedule's status row. Every run
    // of the schedule's CronJob presents it and acts as the person who set the schedule.
    Result<Principal> schedule_principal(const std::string& secret) {
        const auto parsed = parse_agent_credential(secret, agent_schedule_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows = tx.exec_params(
                "SELECT COALESCE(ss.credential_sha256, ''), array_to_json(s.scopes)::text, "
                "COALESCE(users.id::text, ''), COALESCE(users.taap_id, '') FROM "
                "agent_cron_schedules s LEFT JOIN agent_cron_schedule_status ss ON "
                "ss.schedule_id = s.id LEFT JOIN users ON users.id = s.created_by_user_id "
                "WHERE s.id = $1::uuid AND s.desired = 'present' AND users.id IS NOT NULL "
                "AND users.disabled_at IS NULL AND users.deleted_at IS NULL",
                parsed->first);
            if (rows.empty() || !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                                         rows[0][0].as<std::string>())) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            if (rows[0][3].as<std::string>().empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "schedule identity is unavailable"});
            }
            return Principal{
                rows[0][2].as<std::string>(),
                rows[0][3].as<std::string>(),
                "schedule:" + parsed->first,
                "",
                "lake",
                "agent_schedule",
                nlohmann::json::parse(rows[0][1].as<std::string>()).get<std::vector<std::string>>(),
                std::nullopt};
        });
    }

    Result<AgentCronSchedule> create_agent_cron_schedule(const NewAgentCronSchedule& in) override {
        if (in.agent_id.empty() || in.cron.empty() || in.scopes.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent schedule is invalid"});
        }
        return with_tx<AgentCronSchedule>([&](pqxx::work& tx) -> Result<AgentCronSchedule> {
            if (auto active = active_delegator(tx, in.created_by_user_id); !active) {
                return std::unexpected(active.error());
            }
            const pqxx::result customers =
                tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", in.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
            }
            const std::string id = new_uuid_v4();
            tx.exec_params(
                "INSERT INTO agent_cron_schedules (id, customer_id, agent_id, agent_name, cron, "
                "time_zone, description, input, scopes, slug, created_by_user_id, "
                "created_by_taap_id, workspace_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
                "$7, $8::jsonb, ARRAY(SELECT jsonb_array_elements_text($9::jsonb)), $10, "
                "NULLIF($11, '')::uuid, $12, NULLIF($13, '')::uuid)",
                id,
                in.customer_id,
                in.agent_id,
                in.agent_name,
                in.cron,
                in.time_zone.empty() ? std::string("Etc/UTC") : in.time_zone,
                in.description,
                in.input.dump(),
                nlohmann::json(in.scopes).dump(),
                managed_service_slug("as", id),
                in.created_by_user_id,
                in.created_by_taap_id,
                in.workspace_id);
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_cron_schedule(tx, id);
        });
    }

    Result<AgentCronSchedule> get_agent_cron_schedule(const std::string& id) override {
        return with_tx<AgentCronSchedule>(
            [&](pqxx::work& tx) -> Result<AgentCronSchedule> { return one_cron_schedule(tx, id); });
    }

    Result<std::vector<AgentCronSchedule>>
    list_agent_cron_schedules(const std::string& customer_id,
                              const std::string& agent_id,
                              const std::string& workspace_id) override {
        return with_tx<std::vector<AgentCronSchedule>>(
            [&](pqxx::work& tx) -> Result<std::vector<AgentCronSchedule>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("SELECT ") + cron_schedule_columns +
                        " FROM agent_cron_schedules s LEFT JOIN agent_cron_schedule_status ss ON "
                        "ss.schedule_id = s.id WHERE s.customer_id = $1::uuid AND s.desired = "
                        "'present' AND ($2 = '' OR s.agent_id = $2) AND ($3 = '' OR "
                        "s.workspace_id = NULLIF($3, '')::uuid) ORDER BY s.created_at, s.id",
                    customer_id,
                    agent_id,
                    workspace_id);
                std::vector<AgentCronSchedule> schedules;
                for (const pqxx::row& row : rows) {
                    schedules.push_back(cron_schedule_from(row));
                }
                return schedules;
            });
    }

    Result<AgentCronSchedule> withdraw_agent_cron_schedule(const std::string& id) override {
        return with_tx<AgentCronSchedule>([&](pqxx::work& tx) -> Result<AgentCronSchedule> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE agent_cron_schedules SET desired = 'absent', generation = generation + 1, "
                "updated_at = now() WHERE id = $1::uuid AND desired = 'present' RETURNING id",
                id);
            if (rows.empty()) {
                const Result<AgentCronSchedule> existing = one_cron_schedule(tx, id);
                if (!existing) {
                    return existing;
                }
                return std::unexpected(StoreError{"conflict", "the schedule is already removed"});
            }
            tx.exec_params("SELECT pg_notify('lake_services', $1)", id);
            return one_cron_schedule(tx, id);
        });
    }

    Result<std::vector<AgentLaunch>> list_agent_cron_runs(const std::string& customer_id,
                                                          const std::string& agent_id,
                                                          int limit,
                                                          const std::string& workspace_id) override {
        return with_tx<std::vector<AgentLaunch>>(
            [&](pqxx::work& tx) -> Result<std::vector<AgentLaunch>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT r.id::text, s.customer_id::text, s.agent_id, s.agent_name, r.job_name, "
                    "s.input::text, COALESCE(s.created_by_user_id::text, ''), "
                    "s.created_by_taap_id, to_char(r.started_at AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), to_char(r.updated_at AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), r.phase, r.message, r.exit_code, "
                    "r.result::text, COALESCE(s.workspace_id::text, ''), "
                    "COALESCE(to_char(r.finished_at AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') FROM agent_cron_runs r "
                    "JOIN agent_cron_schedules s ON s.id = r.schedule_id WHERE s.customer_id = "
                    "$1::uuid AND ($2 = '' OR s.agent_id = $2) AND ($4 = '' OR s.workspace_id = "
                    "NULLIF($4, '')::uuid) ORDER BY r.started_at DESC, r.id LIMIT $3",
                    customer_id,
                    agent_id,
                    limit > 0 ? limit : 100,
                    workspace_id);
                std::vector<AgentLaunch> runs;
                for (const pqxx::row& row : rows) {
                    AgentLaunch run;
                    run.id = row[0].as<std::string>();
                    run.customer_id = row[1].as<std::string>();
                    run.agent_id = row[2].as<std::string>();
                    run.agent_name = row[3].as<std::string>();
                    run.slug = row[4].as<std::string>();
                    run.mode = "job";
                    run.trigger = "schedule";
                    run.desired = "present";
                    run.input = nlohmann::json::parse(row[5].as<std::string>(), nullptr, false);
                    run.created_by_user_id = row[6].as<std::string>();
                    run.created_by_taap_id = row[7].as<std::string>();
                    run.created_at = row[8].as<std::string>();
                    run.updated_at = row[9].as<std::string>();
                    run.phase = row[10].as<std::string>();
                    run.message = row[11].as<std::string>();
                    if (!row[12].is_null()) {
                        run.exit_code = row[12].as<int>();
                    }
                    run.result = nlohmann::json::parse(row[13].as<std::string>(), nullptr, false);
                    run.workspace_id = row[14].as<std::string>();
                    // When it finished (lake#658: a finished run's duration is charged).
                    run.status_updated_at = row[15].as<std::string>();
                    run.observed_generation = 1; // the operator wrote the row
                    runs.push_back(std::move(run));
                }
                return runs;
            });
    }

    // The dispatch half of a task (lake#374, lake#375). Its content is the task's record in its
    // workspace (migration 0070 dropped the retired body, message and result columns); `notice`
    // here is lake's own notice.
    static constexpr const char* task_columns =
        "t.id::text, t.customer_id::text, t.agent, t.kind, COALESCE(t.conversation_id::text, ''), "
        "COALESCE(t.parent_task_id::text, ''), array_to_json(t.scopes)::text, t.deadline_seconds, "
        "t.desired, COALESCE(t.requested_by_user_id::text, ''), t.requested_by_taap_id, "
        "to_char(t.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(ts.phase, ''), COALESCE(ts.notice, ''), "
        "COALESCE(to_char(ts.started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "''), "
        "COALESCE(to_char(ts.finished_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "''), "
        "COALESCE(ts.credential_sha256, ''), "
        "COALESCE(ts.events_from, 0), COALESCE(t.workspace_id::text, ''), "
        "COALESCE(ts.lease_seconds, 600), COALESCE(ts.attempts, 1), "
        "COALESCE(to_char(ts.lease_until AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), ''), COALESCE(ts.retry_notice_pending, false)";
    static constexpr int task_column_count = 23;

    static Result<AgentTask> one_task(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            std::string("SELECT ") + task_columns +
                " FROM agent_tasks t LEFT JOIN agent_task_status ts ON ts.task_id = t.id "
                "WHERE t.id = $1::uuid",
            id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no agent task " + id});
        }
        return task_from(rows[0]);
    }

    static AgentTask task_from(const pqxx::row& row) {
        AgentTask task;
        task.id = row[0].as<std::string>();
        task.customer_id = row[1].as<std::string>();
        task.agent = row[2].as<std::string>();
        task.kind = row[3].as<std::string>();
        task.conversation_id = row[4].as<std::string>();
        task.parent_task_id = row[5].as<std::string>();
        task.scopes =
            nlohmann::json::parse(row[6].as<std::string>()).get<std::vector<std::string>>();
        task.deadline_seconds = row[7].as<int>();
        task.desired = row[8].as<std::string>();
        task.requested_by_user_id = row[9].as<std::string>();
        task.requested_by_taap_id = row[10].as<std::string>();
        task.created_at = row[11].as<std::string>();
        task.phase = row[12].as<std::string>();
        task.message = row[13].as<std::string>();
        task.started_at = row[14].as<std::string>();
        task.finished_at = row[15].as<std::string>();
        task.credential_sha256 = row[16].as<std::string>();
        task.events_from = row[17].as<std::uint64_t>();
        task.workspace_id = row[18].as<std::string>();
        task.lease_seconds = row[19].as<int>();
        task.attempts = row[20].as<int>();
        task.lease_until = row[21].as<std::string>();
        task.retry_notice_pending = row[22].as<bool>();
        return task;
    }

    Result<std::vector<AgentTaskStreamScope>>
    list_agent_task_stream_scopes(const std::string& customer_id) override {
        return with_tx<std::vector<AgentTaskStreamScope>>(
            [&](pqxx::work& tx) -> Result<std::vector<AgentTaskStreamScope>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT t.id::text, t.agent, "
                    "COALESCE(to_char(ts.finished_at AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), COALESCE(t.workspace_id::text, '') "
                    "FROM agent_tasks t LEFT JOIN agent_task_status ts ON ts.task_id = t.id "
                    "WHERE t.customer_id = $1::uuid ORDER BY t.created_at, t.id",
                    customer_id);
                std::vector<AgentTaskStreamScope> tasks;
                tasks.reserve(rows.size());
                for (const pqxx::row& row : rows) {
                    tasks.push_back({row[0].as<std::string>(), row[1].as<std::string>(),
                                     row[2].as<std::string>(), row[3].as<std::string>()});
                }
                return tasks;
            });
    }

    Result<AgentTask> create_agent_task(const AgentTask& in) override {
        const auto workspace = get_workspace(in.workspace_id);
        if (!workspace || workspace->customer_id != in.customer_id ||
            workspace->desired == "deleted") {
            return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
        }
        if (in.agent.empty() || in.scopes.empty() || (!in.id.empty() && !agent_task_uuid(in.id)) ||
            (!in.parent_task_id.empty() && !agent_task_uuid(in.parent_task_id))) {
            return std::unexpected(StoreError{"invalid_input", "agent task is invalid"});
        }
        return with_tx<AgentTask>([&](pqxx::work& tx) -> Result<AgentTask> {
            if (auto active = active_delegator(tx, in.requested_by_user_id); !active) {
                return std::unexpected(active.error());
            }
            // The request, its message and answer go to the task's workspace (lake#374): the row
            // keeps the default empty body.
            const std::string id = in.id.empty() ? new_uuid_v4() : in.id;
            tx.exec_params(
                "INSERT INTO agent_tasks (id, customer_id, agent, kind, conversation_id, "
                "parent_task_id, scopes, deadline_seconds, requested_by_user_id, "
                "requested_by_taap_id, workspace_id) VALUES "
                "($1::uuid, $2::uuid, $3, $4, NULLIF($5, '')::uuid, NULLIF($6, '')::uuid, "
                "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, NULLIF($9, '')::uuid, "
                "$10, $11::uuid)",
                id,
                in.customer_id,
                in.agent,
                in.kind.empty() ? std::string("api") : in.kind,
                in.conversation_id,
                in.parent_task_id,
                nlohmann::json(in.scopes).dump(),
                in.deadline_seconds,
                in.requested_by_user_id,
                in.requested_by_taap_id,
                in.workspace_id);
            return one_task(tx, id);
        });
    }

    Result<AgentTask> latest_agent_task(const std::string& customer_id,
                                        const std::string& kind,
                                        const std::string& workspace_id) override {
        return with_tx<AgentTask>([&](pqxx::work& tx) -> Result<AgentTask> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + task_columns +
                    " FROM agent_tasks t LEFT JOIN agent_task_status ts ON ts.task_id = t.id "
                    "WHERE t.customer_id = $1::uuid AND t.kind = $2 AND t.workspace_id = $3::uuid "
                    "ORDER BY t.created_at DESC "
                    "LIMIT 1",
                customer_id,
                kind, workspace_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no " + kind + " task"});
            }
            return task_from(rows[0]);
        });
    }

    Result<AgentTask> latest_agent_conversation_task(const std::string& customer_id,
                                                     const std::string& conversation_id,
                                                     const std::string& agent,
                                                     const std::string& kind,
                                                     const std::string& workspace_id) override {
        return with_tx<AgentTask>([&](pqxx::work& tx) -> Result<AgentTask> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + task_columns +
                    " FROM agent_tasks t LEFT JOIN agent_task_status ts ON ts.task_id = t.id "
                    "WHERE t.customer_id = $1::uuid AND t.conversation_id = $2::uuid "
                    "AND t.agent = $3 AND t.kind = $4 AND t.workspace_id = $5::uuid "
                    "ORDER BY t.created_at DESC "
                    "LIMIT 1",
                customer_id,
                conversation_id,
                agent,
                kind,
                workspace_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no earlier turn in this conversation"});
            }
            return task_from(rows[0]);
        });
    }

    Result<std::vector<AgentTask>>
    list_conversation_agent_tasks(const std::string& customer_id,
                                  const std::string& workspace_id,
                                  const std::string& conversation_id,
                                  const std::string& requested_by_user_id) override {
        if (!agent_task_uuid(conversation_id) || !agent_task_uuid(requested_by_user_id) ||
            !agent_task_uuid(workspace_id)) {
            return std::vector<AgentTask>{};
        }
        return with_tx<std::vector<AgentTask>>([&](pqxx::work& tx) -> Result<std::vector<AgentTask>> {
            // The conversation's turns, then every task they asked for at any depth (lake#451).
            const pqxx::result rows = tx.exec_params(
                std::string("WITH RECURSIVE started(id) AS ("
                            "SELECT id FROM agent_tasks WHERE customer_id = $1::uuid "
                            "AND workspace_id = $2::uuid AND conversation_id = $3::uuid "
                            "AND requested_by_user_id = $4::uuid AND kind IN ('ask', 'factory') "
                            "UNION SELECT c.id FROM agent_tasks c JOIN started s "
                            "ON c.parent_task_id = s.id WHERE c.customer_id = $1::uuid "
                            "AND c.workspace_id = $2::uuid) SELECT ") +
                    task_columns +
                    " FROM agent_tasks t LEFT JOIN agent_task_status ts ON ts.task_id = t.id "
                    "WHERE t.id IN (SELECT id FROM started) ORDER BY t.created_at, t.id",
                customer_id,
                workspace_id,
                conversation_id,
                requested_by_user_id);
            std::vector<AgentTask> tasks;
            tasks.reserve(rows.size());
            for (const pqxx::row& row : rows) {
                tasks.push_back(task_from(row));
            }
            return tasks;
        });
    }

    Result<std::size_t> delete_agent_tasks(const std::string& customer_id,
                                           const std::vector<std::string>& ids) override {
        std::vector<std::string> valid;
        for (const std::string& id : ids) {
            if (agent_task_uuid(id)) {
                valid.push_back(id);
            }
        }
        if (valid.empty() || !agent_task_uuid(customer_id)) {
            return std::size_t{0};
        }
        return with_tx<std::size_t>([&](pqxx::work& tx) -> Result<std::size_t> {
            const pqxx::result rows = tx.exec_params(
                "DELETE FROM agent_tasks WHERE customer_id = $1::uuid AND id IN "
                "(SELECT jsonb_array_elements_text($2::jsonb)::uuid) RETURNING id",
                customer_id,
                nlohmann::json(valid).dump());
            return static_cast<std::size_t>(rows.size());
        });
    }

    Result<AgentTask> claim_agent_task(const std::string& id,
                                       const std::string& agent,
                                       const std::string& credential_sha256,
                                       std::uint64_t events_from,
                                       int lease_seconds) override {
        if (lease_seconds < 60 || lease_seconds > 3600) {
            return std::unexpected(
                StoreError{"invalid_input", "agent lease must be 60..3600 seconds"});
        }
        return with_tx<AgentTask>([&](pqxx::work& tx) -> Result<AgentTask> {
            const auto task = one_task(tx, id);
            if (!task) {
                return std::unexpected(task.error());
            }
            if (auto active = active_delegator(tx, task->requested_by_user_id); !active) {
                return std::unexpected(active.error());
            }
            // The unique row and conditional update serialize first claims and reclaims.
            const pqxx::result claimed = tx.exec_params(
                "INSERT INTO agent_task_status (task_id, phase, credential_sha256, started_at, "
                "events_from, lease_seconds, lease_until) "
                "SELECT t.id, 'Running', $3, now(), $4, $5::integer, "
                "now() + make_interval(secs => $5::integer) "
                "FROM agent_tasks t WHERE "
                "t.id = $1::uuid AND t.agent = $2 AND t.desired = 'present' ON CONFLICT (task_id) "
                "DO UPDATE SET phase = 'Running', credential_sha256 = EXCLUDED.credential_sha256, "
                "started_at = now(), updated_at = now(), lease_seconds = EXCLUDED.lease_seconds, "
                "lease_until = EXCLUDED.lease_until WHERE agent_task_status.phase = '' "
                "AND NOT agent_task_status.retry_notice_pending RETURNING task_id",
                id,
                agent,
                credential_sha256,
                static_cast<std::int64_t>(events_from),
                lease_seconds);
            if (claimed.empty()) {
                if (!one_task(tx, id)) {
                    return std::unexpected(StoreError{"not_found", "no agent task " + id});
                }
                return std::unexpected(
                    StoreError{"conflict", "the task is not waiting for " + agent});
            }
            return one_task(tx, id);
        });
    }

    Result<void> renew_agent_task_lease(const std::string& id,
                                        const std::string& credential_sha256) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const auto task = one_task(tx, id);
            if (!task) {
                return std::unexpected(task.error());
            }
            if (task->desired != "present") {
                return std::unexpected(
                    StoreError{"conflict", "the task claim is no longer active"});
            }
            if (auto active = active_delegator(tx, task->requested_by_user_id); !active) {
                return std::unexpected(active.error());
            }
            const auto rows = tx.exec_params(
                "UPDATE agent_task_status SET lease_until = now() + make_interval(secs => "
                "lease_seconds), "
                "updated_at = now() WHERE task_id = $1::uuid AND phase = 'Running' "
                "AND credential_sha256 = $2 AND credential_sha256 <> '' RETURNING task_id",
                id,
                credential_sha256);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "the task claim is no longer active"});
            }
            return {};
        });
    }

    Result<void> mark_agent_task_notice_published(const std::string& id,
                                                  int attempts,
                                                  const std::string& phase) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params(
                "UPDATE agent_task_status SET retry_notice_pending = false, updated_at = now() "
                "WHERE task_id = $1::uuid AND attempts = $2 AND phase = $3 AND "
                "retry_notice_pending",
                id,
                attempts,
                phase);
            return {};
        });
    }

    Result<AgentTask> record_agent_task_event(const std::string& id,
                                              const std::string& agent,
                                              const std::string& phase) override {
        return with_tx<AgentTask>([&](pqxx::work& tx) -> Result<AgentTask> {
            const bool finishing = phase != "Running";
            const pqxx::result updated = tx.exec_params(
                "UPDATE agent_task_status ts SET phase = $3, notice = '', "
                "finished_at = CASE WHEN $4 THEN now() ELSE "
                "NULL END, credential_sha256 = CASE WHEN $4 THEN '' ELSE ts.credential_sha256 END, "
                "lease_until = CASE WHEN $4 THEN NULL ELSE now() + "
                "make_interval(secs => ts.lease_seconds) END, "
                "updated_at = now() FROM agent_tasks t WHERE ts.task_id = t.id AND "
                "t.id = $1::uuid AND t.agent = $2 AND ts.phase = 'Running' AND ts.finished_at "
                "IS NULL RETURNING ts.task_id",
                id,
                agent,
                phase,
                finishing);
            if (updated.empty()) {
                return std::unexpected(
                    StoreError{"conflict", "the task is not running on " + agent});
            }
            return one_task(tx, id);
        });
    }

    Result<void> fail_agent_task(const std::string& id, const std::string& notice) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params(
                "INSERT INTO agent_task_status (task_id, phase, notice, finished_at) "
                "SELECT id, 'Failed', $2, now() FROM agent_tasks WHERE id = $1::uuid "
                "AND created_at + make_interval(secs => deadline_seconds) <= now() "
                "ON CONFLICT (task_id) DO UPDATE SET phase = "
                "'Failed', notice = EXCLUDED.notice, finished_at = now(), credential_sha256 = "
                "'', lease_until = NULL, updated_at = now() "
                "WHERE agent_task_status.phase = '' AND NOT agent_task_status.retry_notice_pending "
                "AND agent_task_status.updated_at + (SELECT make_interval(secs => "
                "deadline_seconds) "
                "FROM agent_tasks WHERE id = $1::uuid) <= now()",
                id,
                notice);
            return {};
        });
    }

    Result<void> mark_agent_task_published(const std::string& id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("UPDATE agent_tasks SET published_at = now() WHERE id = $1::uuid", id);
            return {};
        });
    }

    Result<StaleAgentTasks> stale_agent_tasks(std::chrono::seconds republish_after) override {
        return with_tx<StaleAgentTasks>([&](pqxx::work& tx) -> Result<StaleAgentTasks> {
            // This indexed update owns the transition across replicas; no per-task read/write race.
            tx.exec(
                "UPDATE agent_task_status SET phase = CASE WHEN attempts < 3 THEN '' ELSE 'Failed' "
                "END, "
                "notice = CASE WHEN attempts < 3 THEN 'the agent stopped responding; retrying "
                "(attempt ' "
                "|| (attempts + 1)::text || ' of 3)' ELSE 'the agent stopped responding' END, "
                "finished_at = CASE WHEN attempts >= 3 THEN now() ELSE NULL END, "
                "attempts = LEAST(attempts + 1, 3), lease_until = NULL, credential_sha256 = '', "
                "retry_notice_pending = true, updated_at = now() "
                "WHERE phase = 'Running' AND lease_until < now() RETURNING task_id");
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + task_columns +
                    ", (COALESCE(ts.updated_at, t.created_at) + make_interval(secs => "
                    "t.deadline_seconds) > now()) AS unclaimed FROM agent_tasks t LEFT JOIN "
                    "agent_task_status ts ON ts.task_id = t.id WHERE t.desired = 'present' AND "
                    "(ts.retry_notice_pending OR (COALESCE(ts.phase, '') = '' AND ("
                    "COALESCE(ts.updated_at, t.created_at) + make_interval(secs => "
                    "t.deadline_seconds) <= now() OR COALESCE(t.published_at, t.created_at) + "
                    "make_interval(secs => $1) <= now()))) ORDER BY t.created_at LIMIT 200",
                static_cast<int>(republish_after.count()));
            StaleAgentTasks out;
            for (const pqxx::row& row : rows) {
                AgentTask task = task_from(row);
                if (task.retry_notice_pending) {
                    out.reclaimed.push_back(std::move(task));
                } else {
                    (row[task_column_count].as<bool>() ? out.unclaimed : out.overdue)
                        .push_back(std::move(task));
                }
            }
            return out;
        });
    }

    Result<AgentTask> get_agent_task(const std::string& id) override {
        return with_tx<AgentTask>(
            [&](pqxx::work& tx) -> Result<AgentTask> { return one_task(tx, id); });
    }

    // A task credential: as a launch credential, against the task's status row, acting as the
    // person who asked.
    Result<Principal> task_principal(const std::string& secret) {
        const auto parsed = parse_agent_credential(secret, agent_task_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows = tx.exec_params(
                "SELECT COALESCE(ts.credential_sha256, ''), array_to_json(t.scopes)::text, "
                "COALESCE(users.id::text, ''), COALESCE(users.taap_id, '') FROM agent_tasks t "
                "LEFT JOIN agent_task_status ts ON ts.task_id = t.id LEFT JOIN users ON users.id = "
                "t.requested_by_user_id WHERE t.id = $1::uuid AND t.desired = 'present' "
                "AND users.id IS NOT NULL AND users.disabled_at IS NULL AND users.deleted_at IS "
                "NULL",
                parsed->first);
            if (rows.empty() || !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                                         rows[0][0].as<std::string>())) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            if (rows[0][3].as<std::string>().empty()) {
                return std::unexpected(StoreError{"unauthorized", "task identity is unavailable"});
            }
            return Principal{
                rows[0][2].as<std::string>(),
                rows[0][3].as<std::string>(),
                "task:" + parsed->first,
                "",
                "lake",
                "agent_task",
                nlohmann::json::parse(rows[0][1].as<std::string>()).get<std::vector<std::string>>(),
                std::nullopt,
                parsed->first,
                "",
                "",
                "",
                rows[0][0].as<std::string>()};
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

    Result<std::vector<UsageLimit>> usage_limits(const std::string& customer_id) override {
        return with_tx<std::vector<UsageLimit>>(
            [&](pqxx::work& tx) -> Result<std::vector<UsageLimit>> {
                const pqxx::result customers =
                    tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id);
                if (customers.empty()) {
                    return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
                }
                const pqxx::result rows = tx.exec_params(
                    "SELECT dimension, limit_value, warn_percent, "
                    "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                    "FROM customer_usage_limits WHERE customer_id = $1::uuid ORDER BY dimension",
                    customer_id);
                std::vector<UsageLimit> limits;
                for (const auto& row : rows) {
                    limits.push_back({customer_id,
                                      row[0].as<std::string>(),
                                      row[1].as<std::uint64_t>(),
                                      row[2].as<int>(),
                                      row[3].as<std::string>()});
                }
                return limits;
            });
    }

    Result<UsageLimit> set_usage_limit(const UsageLimit& limit) override {
        if (limit.dimension.empty() || limit.warn_percent < 1 || limit.warn_percent > 100) {
            return std::unexpected(StoreError{"invalid_input",
                                              "a dimension and a warning "
                                              "percentage from 1 to 100 are "
                                              "required"});
        }
        return with_tx<UsageLimit>([&](pqxx::work& tx) -> Result<UsageLimit> {
            const pqxx::result customers =
                tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", limit.customer_id);
            if (customers.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + limit.customer_id});
            }
            if (limit.limit == 0) {
                tx.exec_params("DELETE FROM customer_usage_limits WHERE customer_id = $1::uuid "
                               "AND dimension = $2",
                               limit.customer_id,
                               limit.dimension);
                return limit;
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO customer_usage_limits (customer_id, dimension, limit_value, "
                "warn_percent) VALUES ($1::uuid, $2, $3, $4) ON CONFLICT (customer_id, dimension) "
                "DO UPDATE SET limit_value = EXCLUDED.limit_value, warn_percent = "
                "EXCLUDED.warn_percent, updated_at = now() RETURNING "
                "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",
                limit.customer_id,
                limit.dimension,
                limit.limit,
                limit.warn_percent);
            UsageLimit stored = limit;
            stored.updated_at = rows[0][0].as<std::string>();
            return stored;
        });
    }

    // ---- billing (migration 0073, lake#564, docs/api/billing.md) ----

    static constexpr const char* utc_text_sql = "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'";

    static std::string utc_column(const std::string& column) {
        return "COALESCE(to_char(" + column + " AT TIME ZONE 'UTC', " + utc_text_sql + "), '')";
    }

    Result<std::vector<BillingPlan>> billing_plans() override {
        return with_tx<std::vector<BillingPlan>>(
            [&](pqxx::work& tx) -> Result<std::vector<BillingPlan>> {
                const pqxx::result rows =
                    tx.exec("SELECT id, name, description, currency, base_micros, active, "
                            "position, " +
                            utc_column("created_at") + ", " + utc_column("updated_at") +
                            " FROM billing_plans ORDER BY position, id");
                std::vector<BillingPlan> plans;
                for (const auto& row : rows) {
                    BillingPlan plan;
                    plan.id = row[0].as<std::string>();
                    plan.name = row[1].as<std::string>();
                    plan.description = row[2].as<std::string>();
                    plan.currency = row[3].as<std::string>();
                    plan.base_micros = row[4].as<std::uint64_t>();
                    plan.active = row[5].as<bool>();
                    plan.position = row[6].as<int>();
                    plan.created_at = row[7].as<std::string>();
                    plan.updated_at = row[8].as<std::string>();
                    plans.push_back(std::move(plan));
                }
                const pqxx::result prices =
                    tx.exec("SELECT plan_id, dimension, unit_micros, per, included, class "
                            "FROM billing_plan_prices ORDER BY plan_id, dimension, class");
                for (const auto& row : prices) {
                    const std::string plan_id = row[0].as<std::string>();
                    for (BillingPlan& plan : plans) {
                        if (plan.id == plan_id) {
                            plan.prices.push_back({row[1].as<std::string>(),
                                                   row[2].as<std::uint64_t>(),
                                                   row[3].as<std::uint64_t>(),
                                                   row[4].as<std::uint64_t>(),
                                                   row[5].as<std::string>()});
                        }
                    }
                }
                return plans;
            });
    }

    Result<BillingPlan> put_billing_plan(const BillingPlan& plan) override {
        std::set<std::pair<std::string, std::string>> seen;
        for (const BillingPrice& price : plan.prices) {
            if (price.dimension.empty() || price.per == 0 ||
                !seen.insert({price.dimension, price.class_name}).second) {
                return std::unexpected(
                    StoreError{"invalid_input",
                               "each price needs its own dimension and class and a `per` above 0"});
            }
        }
        auto written = with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            tx.exec_params(
                "INSERT INTO billing_plans (id, name, description, currency, base_micros, active, "
                "position) VALUES ($1, $2, $3, $4, $5, $6, $7) ON CONFLICT (id) DO UPDATE SET "
                "name = EXCLUDED.name, description = EXCLUDED.description, currency = "
                "EXCLUDED.currency, base_micros = EXCLUDED.base_micros, active = EXCLUDED.active, "
                "position = EXCLUDED.position, updated_at = now()",
                plan.id,
                plan.name,
                plan.description,
                plan.currency,
                plan.base_micros,
                plan.active,
                plan.position);
            tx.exec_params("DELETE FROM billing_plan_prices WHERE plan_id = $1", plan.id);
            for (const BillingPrice& price : plan.prices) {
                tx.exec_params("INSERT INTO billing_plan_prices (plan_id, dimension, unit_micros, "
                               "per, included, class) VALUES ($1, $2, $3, $4, $5, $6)",
                               plan.id,
                               price.dimension,
                               price.unit_micros,
                               price.per,
                               price.included,
                               price.class_name);
            }
            return true;
        });
        if (!written) {
            return std::unexpected(written.error());
        }
        auto plans = billing_plans();
        if (!plans) {
            return std::unexpected(plans.error());
        }
        for (BillingPlan& stored : *plans) {
            if (stored.id == plan.id) {
                return std::move(stored);
            }
        }
        return std::unexpected(StoreError{"store_error", "plan " + plan.id + " was not kept"});
    }

    Result<std::vector<WorkloadResources>> workload_resources() override {
        return with_tx<std::vector<WorkloadResources>>(
            [&](pqxx::work& tx) -> Result<std::vector<WorkloadResources>> {
                const pqxx::result rows = tx.exec(
                    "SELECT object_kind, object_id::text, pods, cpu_millis, memory_bytes, gpus, "
                    "gpu_class, vram_bytes, " +
                    utc_column("observed_at") +
                    " FROM workload_resources ORDER BY object_kind, object_id");
                std::vector<WorkloadResources> out;
                for (const auto& row : rows) {
                    out.push_back({row[0].as<std::string>(),
                                   row[1].as<std::string>(),
                                   row[2].as<int>(),
                                   row[3].as<std::uint64_t>(),
                                   row[4].as<std::uint64_t>(),
                                   row[5].as<int>(),
                                   row[6].as<std::string>(),
                                   row[7].as<std::uint64_t>(),
                                   row[8].as<std::string>()});
                }
                return out;
            });
    }

    Result<void> put_workload_resources(const WorkloadResources& row) override {
        auto written = with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            tx.exec_params(
                "INSERT INTO workload_resources (object_kind, object_id, pods, cpu_millis, "
                "memory_bytes, gpus, gpu_class, vram_bytes) VALUES ($1, $2::uuid, $3, $4, $5, $6, "
                "$7, $8) ON CONFLICT (object_kind, object_id) DO UPDATE SET pods = EXCLUDED.pods, "
                "cpu_millis = EXCLUDED.cpu_millis, memory_bytes = EXCLUDED.memory_bytes, gpus = "
                "EXCLUDED.gpus, gpu_class = EXCLUDED.gpu_class, vram_bytes = EXCLUDED.vram_bytes, "
                "observed_at = now()",
                row.object_kind,
                row.object_id,
                row.pods,
                row.cpu_millis,
                row.memory_bytes,
                row.gpus,
                row.gpu_class,
                row.vram_bytes);
            return true;
        });
        if (!written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    Result<void> put_mcp_connection_dispatch(const McpConnectionDispatch& row) override {
        if (!agent_task_uuid(row.customer_id) || !agent_task_uuid(row.workspace_id) ||
            row.connection_id.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "not an MCP connection dispatch row"});
        }
        auto written = with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            if (!row.streams && row.exported_digest.empty()) {
                tx.exec_params("DELETE FROM mcp_connection_dispatch WHERE workspace_id = $1::uuid "
                               "AND connection_id = $2",
                               row.workspace_id,
                               row.connection_id);
                return true;
            }
            const pqxx::result kept = tx.exec_params(
                "INSERT INTO mcp_connection_dispatch (customer_id, workspace_id, connection_id, "
                "streams, exported_digest) SELECT w.customer_id, w.id, $3, $4, $5 FROM workspaces "
                "w WHERE w.id = $2::uuid AND w.customer_id = $1::uuid ON CONFLICT (workspace_id, "
                "connection_id) DO UPDATE SET streams = EXCLUDED.streams, exported_digest = "
                "EXCLUDED.exported_digest, updated_at = now() RETURNING 1",
                row.customer_id,
                row.workspace_id,
                row.connection_id,
                row.streams,
                row.exported_digest);
            if (kept.empty()) {
                return std::unexpected(StoreError{"not_found", "workspace not found"});
            }
            return true;
        });
        if (!written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    Result<void> remove_mcp_connection_dispatch(const std::string& workspace_id,
                                                const std::string& connection_id) override {
        if (!agent_task_uuid(workspace_id)) {
            return {};
        }
        auto removed = with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            tx.exec_params("DELETE FROM mcp_connection_dispatch WHERE workspace_id = $1::uuid AND "
                           "connection_id = $2",
                           workspace_id,
                           connection_id);
            return true;
        });
        if (!removed) {
            return std::unexpected(removed.error());
        }
        return {};
    }

    static std::vector<McpConnectionDispatch> dispatch_rows(const pqxx::result& rows) {
        std::vector<McpConnectionDispatch> out;
        for (const auto& row : rows) {
            out.push_back({row[0].as<std::string>(),
                           row[1].as<std::string>(),
                           row[2].as<std::string>(),
                           row[3].as<bool>(),
                           row[4].as<std::string>(),
                           row[5].as<std::string>()});
        }
        return out;
    }

    Result<std::vector<McpConnectionDispatch>> mcp_stream_dispatch(int limit) override {
        return with_tx<std::vector<McpConnectionDispatch>>(
            [&](pqxx::work& tx) -> Result<std::vector<McpConnectionDispatch>> {
                return dispatch_rows(tx.exec_params(
                    "SELECT customer_id::text, workspace_id::text, connection_id, streams, "
                    "exported_digest, " +
                        utc_column("updated_at") +
                        " FROM mcp_connection_dispatch WHERE streams ORDER BY workspace_id, "
                        "connection_id LIMIT $1",
                    std::max(limit, 0)));
            });
    }

    Result<std::vector<McpConnectionDispatch>>
    mcp_exported_dispatch(const std::string& workspace_id) override {
        if (!agent_task_uuid(workspace_id)) {
            return std::vector<McpConnectionDispatch>{};
        }
        return with_tx<std::vector<McpConnectionDispatch>>(
            [&](pqxx::work& tx) -> Result<std::vector<McpConnectionDispatch>> {
                return dispatch_rows(tx.exec_params(
                    "SELECT customer_id::text, workspace_id::text, connection_id, streams, "
                    "exported_digest, " +
                        utc_column("updated_at") +
                        " FROM mcp_connection_dispatch WHERE workspace_id = $1::uuid AND "
                        "exported_digest <> '' ORDER BY connection_id",
                    workspace_id));
            });
    }

    Result<std::vector<AppTransfer>> app_transfer(const std::string& since_hour) override {
        return with_tx<std::vector<AppTransfer>>(
            [&](pqxx::work& tx) -> Result<std::vector<AppTransfer>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT app_id::text, workspace_id::text, " + utc_column("hour") +
                        ", in_bytes, out_bytes, " + utc_column("observed_at") +
                        " FROM app_transfer WHERE hour >= $1::timestamptz ORDER BY hour, app_id",
                    since_hour);
                std::vector<AppTransfer> out;
                for (const auto& row : rows) {
                    out.push_back({row[0].as<std::string>(),
                                   row[1].as<std::string>(),
                                   row[2].as<std::string>(),
                                   row[3].as<std::uint64_t>(),
                                   row[4].as<std::uint64_t>(),
                                   row[5].as<std::string>()});
                }
                return out;
            });
    }

    Result<void> add_app_transfer(const AppTransfer& row) override {
        auto written = with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            tx.exec_params(
                "INSERT INTO app_transfer (app_id, workspace_id, hour, in_bytes, out_bytes) "
                "VALUES ($1::uuid, $2::uuid, $3::timestamptz, $4, $5) ON CONFLICT (app_id, hour) "
                "DO UPDATE SET in_bytes = app_transfer.in_bytes + EXCLUDED.in_bytes, out_bytes = "
                "app_transfer.out_bytes + EXCLUDED.out_bytes, observed_at = now()",
                row.app_id,
                row.workspace_id,
                row.hour,
                row.in_bytes,
                row.out_bytes);
            return true;
        });
        if (!written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    static std::string invoice_columns() {
        return "id::text, number, customer_id::text, period, plan_id, plan_name, currency, "
               "total_micros, state, provider, provider_reference, instructions, note, " +
               utc_column("issued_at") + ", " + utc_column("paid_at") + ", " +
               utc_column("voided_at") + ", payment_url, provider_status";
    }

    static Invoice invoice_of(const pqxx::row& row) {
        Invoice invoice;
        invoice.id = row[0].as<std::string>();
        invoice.number = row[1].as<std::uint64_t>();
        invoice.customer_id = row[2].as<std::string>();
        invoice.period = row[3].as<std::string>();
        invoice.plan_id = row[4].as<std::string>();
        invoice.plan_name = row[5].as<std::string>();
        invoice.currency = row[6].as<std::string>();
        invoice.total_micros = row[7].as<std::uint64_t>();
        invoice.state = row[8].as<std::string>();
        invoice.provider = row[9].as<std::string>();
        invoice.provider_reference = row[10].as<std::string>();
        invoice.instructions = row[11].as<std::string>();
        invoice.note = row[12].as<std::string>();
        invoice.issued_at = row[13].as<std::string>();
        invoice.paid_at = row[14].as<std::string>();
        invoice.voided_at = row[15].as<std::string>();
        invoice.payment_url = row[16].as<std::string>();
        invoice.provider_status = row[17].as<std::string>();
        return invoice;
    }

    static void read_lines(pqxx::work& tx, Invoice& invoice) {
        const pqxx::result rows = tx.exec_params(
            "SELECT kind, dimension, description, unit, quantity, included, billable, "
            "unit_micros, per, amount_micros, class FROM billing_invoice_lines WHERE invoice_id = "
            "$1::uuid ORDER BY position",
            invoice.id);
        invoice.lines.clear();
        for (const auto& row : rows) {
            invoice.lines.push_back({row[0].as<std::string>(),
                                     row[1].as<std::string>(),
                                     row[2].as<std::string>(),
                                     row[3].as<std::string>(),
                                     row[4].as<std::uint64_t>(),
                                     row[5].as<std::uint64_t>(),
                                     row[6].as<std::uint64_t>(),
                                     row[7].as<std::uint64_t>(),
                                     row[8].as<std::uint64_t>(),
                                     row[9].as<std::uint64_t>(),
                                     row[10].as<std::string>()});
        }
    }

    static Result<Invoice> invoice_in(pqxx::work& tx, const std::string& id) {
        const pqxx::result rows = tx.exec_params(
            "SELECT " + invoice_columns() + " FROM billing_invoices WHERE id = $1::uuid", id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no invoice " + id});
        }
        Invoice invoice = invoice_of(rows[0]);
        read_lines(tx, invoice);
        return invoice;
    }

    Result<Invoice> create_invoice(const Invoice& invoice) override {
        const std::string id = new_uuid_v4();
        return with_tx<Invoice>([&](pqxx::work& tx) -> Result<Invoice> {
            const pqxx::result existing = tx.exec_params(
                "SELECT number FROM billing_invoices WHERE customer_id = $1::uuid AND period = $2 "
                "AND state <> 'void'",
                invoice.customer_id,
                invoice.period);
            if (!existing.empty()) {
                return std::unexpected(StoreError{"conflict",
                                                  "invoice " + existing[0][0].as<std::string>() +
                                                      " already bills " + invoice.period});
            }
            tx.exec_params("INSERT INTO billing_invoices (id, customer_id, period, plan_id, "
                           "plan_name, currency, total_micros, provider, provider_reference, "
                           "instructions, note, payment_url, provider_status) VALUES ($1::uuid, "
                           "$2::uuid, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)",
                           id,
                           invoice.customer_id,
                           invoice.period,
                           invoice.plan_id,
                           invoice.plan_name,
                           invoice.currency,
                           invoice.total_micros,
                           invoice.provider,
                           invoice.provider_reference,
                           invoice.instructions,
                           invoice.note,
                           invoice.payment_url,
                           invoice.provider_status);
            int position = 0;
            for (const InvoiceLine& line : invoice.lines) {
                tx.exec_params(
                    "INSERT INTO billing_invoice_lines (invoice_id, position, kind, dimension, "
                    "description, unit, quantity, included, billable, unit_micros, per, "
                    "amount_micros, class) VALUES ($1::uuid, $2, $3, $4, $5, $6, $7, $8, $9, $10, "
                    "$11, $12, $13)",
                    id,
                    position++,
                    line.kind,
                    line.dimension,
                    line.description,
                    line.unit,
                    line.quantity,
                    line.included,
                    line.billable,
                    line.unit_micros,
                    line.per,
                    line.amount_micros,
                    line.class_name);
            }
            return invoice_in(tx, id);
        });
    }

    Result<std::vector<Invoice>> list_invoices(const std::string& customer_id) override {
        return with_tx<std::vector<Invoice>>([&](pqxx::work& tx) -> Result<std::vector<Invoice>> {
            const pqxx::result rows =
                tx.exec_params("SELECT " + invoice_columns() +
                                   " FROM billing_invoices WHERE customer_id = $1::uuid "
                                   "ORDER BY period DESC, number DESC",
                               customer_id);
            std::vector<Invoice> out;
            for (const auto& row : rows) {
                Invoice invoice = invoice_of(row);
                read_lines(tx, invoice);
                out.push_back(std::move(invoice));
            }
            return out;
        });
    }

    Result<Invoice> get_invoice(const std::string& id) override {
        return with_tx<Invoice>([&](pqxx::work& tx) { return invoice_in(tx, id); });
    }

    Result<Invoice> set_invoice_state(const std::string& id,
                                      const std::string& state,
                                      const std::string& provider_reference,
                                      const std::string& note) override {
        if (state != "paid" && state != "void") {
            return std::unexpected(StoreError{"invalid_input", "an invoice becomes paid or void"});
        }
        return with_tx<Invoice>([&](pqxx::work& tx) -> Result<Invoice> {
            auto before = invoice_in(tx, id);
            if (!before) {
                return before;
            }
            if (before->state != "due") {
                return std::unexpected(StoreError{"conflict",
                                                  "invoice " + std::to_string(before->number) +
                                                      " is " + before->state + ", not due"});
            }
            tx.exec_params(
                "UPDATE billing_invoices SET state = $2, provider_reference = CASE WHEN $3 = '' "
                "THEN provider_reference ELSE $3 END, note = $4, paid_at = CASE WHEN $2 = 'paid' "
                "THEN now() ELSE paid_at END, voided_at = CASE WHEN $2 = 'void' THEN now() ELSE "
                "voided_at END WHERE id = $1::uuid AND state = 'due'",
                id,
                state,
                provider_reference,
                note);
            return invoice_in(tx, id);
        });
    }

    Result<Invoice> set_invoice_provider_status(const std::string& id,
                                                const std::string& from,
                                                const std::string& to) override {
        return with_tx<Invoice>([&](pqxx::work& tx) -> Result<Invoice> {
            tx.exec_params("UPDATE billing_invoices SET provider_status = $3 WHERE id = $1::uuid "
                           "AND state = 'due' AND provider_status = $2",
                           id,
                           from,
                           to);
            return invoice_in(tx, id);
        });
    }

    // ---- payment providers (migration 0074, lake#620, docs/api/billing.md#stripe) ----

    Result<std::string> billing_provider_customer(const std::string& customer_id,
                                                  const std::string& provider) override {
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            if (tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id)
                    .empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
            }
            const pqxx::result rows = tx.exec_params(
                "SELECT provider_customer_id FROM billing_accounts WHERE customer_id = $1::uuid "
                "AND provider = $2",
                customer_id,
                provider);
            return rows.empty() ? std::string() : rows[0][0].as<std::string>();
        });
    }

    Result<std::string>
    set_billing_provider_customer(const std::string& customer_id,
                                  const std::string& provider,
                                  const std::string& provider_customer_id) override {
        if (provider.empty() || provider_customer_id.empty() || provider_customer_id.size() > 255) {
            return std::unexpected(
                StoreError{"invalid_input", "a provider and its customer id are required"});
        }
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            if (tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id)
                    .empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
            }
            tx.exec_params("INSERT INTO billing_accounts (customer_id, provider, "
                           "provider_customer_id) VALUES ($1::uuid, $2, $3) ON CONFLICT DO NOTHING",
                           customer_id,
                           provider,
                           provider_customer_id);
            return tx
                .exec_params("SELECT provider_customer_id FROM billing_accounts WHERE customer_id "
                             "= $1::uuid AND provider = $2",
                             customer_id,
                             provider)[0][0]
                .as<std::string>();
        });
    }

    Result<std::string>
    replace_billing_provider_customer(const std::string& customer_id,
                                      const std::string& provider,
                                      const std::string& expected_id,
                                      const std::string& provider_customer_id) override {
        if (provider.empty() || expected_id.empty() || provider_customer_id.empty() ||
            provider_customer_id.size() > 255) {
            return std::unexpected(
                StoreError{"invalid_input",
                           "a provider, the kept customer id and its replacement are required"});
        }
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            if (tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id)
                    .empty()) {
                return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
            }
            // The row lock taken by a concurrent update makes this one wait and then match
            // nothing, so the first replacement stays.
            tx.exec_params("UPDATE billing_accounts SET provider_customer_id = $4 WHERE "
                           "customer_id = $1::uuid AND provider = $2 AND provider_customer_id = $3",
                           customer_id,
                           provider,
                           expected_id,
                           provider_customer_id);
            const pqxx::result rows = tx.exec_params(
                "SELECT provider_customer_id FROM billing_accounts WHERE customer_id = $1::uuid "
                "AND provider = $2",
                customer_id,
                provider);
            return rows.empty() ? std::string() : rows[0][0].as<std::string>();
        });
    }

    Result<PaymentEventResult> apply_payment_event(const PaymentEvent& event) override {
        if (event.provider.empty() || event.event_id.empty() || event.event_id.size() > 255 ||
            (!event.state.empty() && event.state != "paid" && event.state != "void")) {
            return std::unexpected(StoreError{"invalid_input",
                                              "a payment event needs its provider, its id and a "
                                              "state of paid or void"});
        }
        return with_tx<PaymentEventResult>([&](pqxx::work& tx) -> Result<PaymentEventResult> {
            PaymentEventResult result;
            const pqxx::result recorded = tx.exec_params(
                "INSERT INTO billing_payment_events (provider, event_id, event_type, reference) "
                "VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING RETURNING 1",
                event.provider,
                event.event_id,
                event.event_type.substr(0, 128),
                event.reference.substr(0, 256));
            if (recorded.empty()) {
                result.duplicate = true;
                return result;
            }
            if (event.reference.empty()) {
                return result;
            }
            const pqxx::result rows = tx.exec_params(
                "SELECT id::text FROM billing_invoices WHERE provider = $1 AND provider_reference "
                "= $2 ORDER BY number DESC LIMIT 1 FOR UPDATE",
                event.provider,
                event.reference);
            if (rows.empty()) {
                return result;
            }
            const std::string id = rows[0][0].as<std::string>();
            auto before = invoice_in(tx, id);
            if (!before) {
                return std::unexpected(before.error());
            }
            result.matched = true;
            if (before->state == "due") {
                const pqxx::result changed = tx.exec_params(
                    "UPDATE billing_invoices SET state = CASE WHEN $2 = '' THEN state ELSE $2 END, "
                    "paid_at = CASE WHEN $2 = 'paid' THEN now() ELSE paid_at END, voided_at = CASE "
                    "WHEN $2 = 'void' THEN now() ELSE voided_at END, note = CASE WHEN $2 <> '' AND "
                    "$4 <> '' THEN $4 ELSE note END, provider_status = CASE WHEN $3 = '' THEN "
                    "provider_status ELSE $3 END WHERE id = $1::uuid AND state = 'due' AND ($2 <> "
                    "'' OR ($3 <> '' AND provider_status <> $3)) RETURNING 1",
                    id,
                    event.state,
                    event.provider_status,
                    event.note);
                result.changed = !changed.empty();
            } else if (!event.state.empty() && event.state == before->state &&
                       !event.provider_status.empty() &&
                       before->provider_status != event.provider_status) {
                tx.exec_params(
                    "UPDATE billing_invoices SET provider_status = $2 WHERE id = $1::uuid",
                    id,
                    event.provider_status);
                result.changed = true;
            }
            tx.exec_params("UPDATE billing_payment_events SET invoice_id = $3::uuid WHERE "
                           "provider = $1 AND event_id = $2",
                           event.provider,
                           event.event_id,
                           id);
            auto after = invoice_in(tx, id);
            if (!after) {
                return std::unexpected(after.error());
            }
            result.invoice = std::move(*after);
            return result;
        });
    }

    // ---- billing contacts (migration 0090, lake#857, docs/api/billing.md#billing-contact) ----

    Result<std::optional<BillingContact>> billing_contact(const std::string& customer_id) override {
        return with_tx<std::optional<BillingContact>>(
            [&](pqxx::work& tx) -> Result<std::optional<BillingContact>> {
                if (tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", customer_id)
                        .empty()) {
                    return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
                }
                const pqxx::result rows = tx.exec_params(
                    "SELECT email, name, line1, line2, city, postal_code, state, country, "
                    "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') "
                    "FROM billing_contacts WHERE customer_id = $1::uuid",
                    customer_id);
                if (rows.empty()) {
                    return std::optional<BillingContact>();
                }
                const pqxx::row row = rows[0];
                return BillingContact{customer_id,
                                      row[0].as<std::string>(),
                                      row[1].as<std::string>(),
                                      row[2].as<std::string>(),
                                      row[3].as<std::string>(),
                                      row[4].as<std::string>(),
                                      row[5].as<std::string>(),
                                      row[6].as<std::string>(),
                                      row[7].as<std::string>(),
                                      row[8].as<std::string>()};
            });
    }

    Result<BillingContact> set_billing_contact(const BillingContact& contact) override {
        if (const auto problem = billing_contact_problem(contact)) {
            return std::unexpected(
                StoreError{"invalid_input",
                           "billing contact " + problem->field + " must be " + problem->message});
        }
        return with_tx<BillingContact>([&](pqxx::work& tx) -> Result<BillingContact> {
            if (tx.exec_params("SELECT 1 FROM customers WHERE id = $1::uuid", contact.customer_id)
                    .empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no customer " + contact.customer_id});
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO billing_contacts (customer_id, email, name, line1, line2, city, "
                "postal_code, state, country) VALUES ($1::uuid, $2, $3, $4, $5, $6, $7, $8, $9) "
                "ON CONFLICT (customer_id) DO UPDATE SET email = EXCLUDED.email, name = "
                "EXCLUDED.name, line1 = EXCLUDED.line1, line2 = EXCLUDED.line2, city = "
                "EXCLUDED.city, postal_code = EXCLUDED.postal_code, state = EXCLUDED.state, "
                "country = EXCLUDED.country, updated_at = now() RETURNING "
                "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')",
                contact.customer_id,
                contact.email,
                contact.name,
                contact.line1,
                contact.line2,
                contact.city,
                contact.postal_code,
                contact.state,
                contact.country);
            BillingContact stored = contact;
            stored.updated_at = rows[0][0].as<std::string>();
            return stored;
        });
    }

    // Cloud orders (lake#831, migration 0089, docs/api/orders.md).
    static constexpr const char* cloud_order_columns =
        "id::text, to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), offer, path, "
        "cloud_provider, region, contact_name, contact_email, company, details, quantity, "
        "amount_cents, currency, included_credit_cents, coalesce(stripe_checkout_session_id, ''), "
        "coalesce(stripe_customer_id, ''), coalesce(stripe_subscription_id, ''), state, "
        "coalesce(host, ''), coalesce(failure_reason, ''), coalesce(claimed_by, ''), "
        "coalesce(to_char(claimed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "coalesce(organization_id::text, ''), coalesce(support_emailed_state, ''), "
        "coalesce(customer_emailed_state, ''), email_attempts, email_error, "
        "coalesce(to_char(email_retry_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "'')";

    static CloudOrder cloud_order_row(const pqxx::row& row) {
        CloudOrder order;
        int column = 0;
        const auto text = [&] { return row[column++].as<std::string>(); };
        order.id = text();
        order.created_at = text();
        order.updated_at = text();
        order.offer = text();
        order.path = text();
        order.cloud_provider = text();
        order.region = text();
        order.contact_name = text();
        order.contact_email = text();
        order.company = text();
        order.details = text();
        order.quantity = row[column++].as<int>();
        order.amount_cents = row[column++].as<std::int64_t>();
        order.currency = text();
        order.included_credit_cents = row[column++].as<std::int64_t>();
        order.stripe_checkout_session_id = text();
        order.stripe_customer_id = text();
        order.stripe_subscription_id = text();
        order.state = text();
        order.host = text();
        order.failure_reason = text();
        order.claimed_by = text();
        order.claimed_at = text();
        order.organization_id = text();
        order.support_emailed_state = text();
        order.customer_emailed_state = text();
        order.email_attempts = row[column++].as<int>();
        order.email_error = text();
        order.email_retry_at = text();
        return order;
    }

    static bool uuid_text(const std::string& id) {
        if (id.size() != 36) {
            return false;
        }
        for (std::size_t index = 0; index < id.size(); ++index) {
            const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
            if (dash ? id[index] != '-'
                     : std::isxdigit(static_cast<unsigned char>(id[index])) == 0) {
                return false;
            }
        }
        return true;
    }

    static Result<CloudOrder>
    cloud_order_in(pqxx::work& tx, const std::string& id, bool for_update = false) {
        const pqxx::result rows = tx.exec_params(std::string("SELECT ") + cloud_order_columns +
                                                     " FROM cloud_orders WHERE id = $1::uuid" +
                                                     (for_update ? " FOR UPDATE" : ""),
                                                 id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "no order " + id});
        }
        return cloud_order_row(rows[0]);
    }

    static void cloud_order_event(pqxx::work& tx,
                                  const std::string& id,
                                  const std::string& from,
                                  const std::string& to,
                                  const std::string& actor,
                                  const std::string& detail) {
        tx.exec_params("INSERT INTO cloud_order_events (order_id, from_state, to_state, actor, "
                       "detail) VALUES ($1::uuid, $2, $3, $4, $5)",
                       id,
                       nullable(from),
                       to,
                       actor,
                       utf8_prefix(detail, 2000));
    }

    Result<CloudOrder> create_cloud_order(const NewCloudOrder& in) override {
        if (in.path != "checkout" && in.path != "order_form") {
            return std::unexpected(
                StoreError{"invalid_input", "an order's path is checkout or order_form"});
        }
        if (!in.organization_id.empty() && !uuid_text(in.organization_id)) {
            return std::unexpected(StoreError{"invalid_input", "organization_id must be a uuid"});
        }
        const std::string state = in.path == "checkout" ? "pending_payment" : "submitted";
        return with_tx<CloudOrder>([&](pqxx::work& tx) -> Result<CloudOrder> {
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO cloud_orders (offer, path, cloud_provider, region, contact_name, "
                "contact_email, company, details, quantity, amount_cents, currency, "
                "included_credit_cents, state, organization_id, email_error) VALUES ($1, $2, $3, "
                "$4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14::uuid, $15) RETURNING id::text",
                in.offer,
                in.path,
                in.cloud_provider,
                in.region,
                in.contact_name,
                in.contact_email,
                in.company,
                in.details,
                in.quantity,
                in.amount_cents,
                in.currency,
                in.included_credit_cents,
                state,
                nullable(in.organization_id),
                utf8_prefix(in.email_error, 1024));
            const std::string id = rows[0][0].as<std::string>();
            cloud_order_event(tx,
                              id,
                              "",
                              state,
                              "lake",
                              in.path == "checkout" ? "ordered from the order sheet, to pay"
                                                    : "order form sent from the order sheet");
            return cloud_order_in(tx, id);
        });
    }

    Result<CloudOrder> get_cloud_order(const std::string& id) override {
        if (!uuid_text(id)) {
            return std::unexpected(StoreError{"not_found", "no order " + id});
        }
        return with_tx<CloudOrder>([&](pqxx::work& tx) { return cloud_order_in(tx, id); });
    }

    Result<CloudOrder> transition_cloud_order(const CloudOrderTransition& change) override {
        if (!uuid_text(change.id)) {
            return std::unexpected(StoreError{"not_found", "no order " + change.id});
        }
        return with_tx<CloudOrder>([&](pqxx::work& tx) -> Result<CloudOrder> {
            auto before = cloud_order_in(tx, change.id, true);
            if (!before) {
                return before;
            }
            if (before->state != change.from) {
                return std::unexpected(StoreError{
                    "conflict", "the order is " + before->state + ", not " + change.from});
            }
            tx.exec_params("UPDATE cloud_orders SET state = $2, stripe_checkout_session_id = "
                           "coalesce($3, stripe_checkout_session_id), host = coalesce($4, host), "
                           "failure_reason = coalesce($5, failure_reason) WHERE id = $1::uuid",
                           change.id,
                           change.to,
                           nullable(change.checkout_session_id),
                           nullable(change.host),
                           nullable(change.failure_reason));
            if (change.to != change.from) {
                cloud_order_event(
                    tx, change.id, change.from, change.to, change.actor, change.detail);
            }
            return cloud_order_in(tx, change.id);
        });
    }

    Result<CloudOrderPaymentResult>
    apply_cloud_order_payment(const CloudOrderPayment& payment) override {
        if (payment.provider.empty() || payment.event_id.empty() || payment.event_id.size() > 255 ||
            (payment.state != "paid" && payment.state != "cancelled")) {
            return std::unexpected(StoreError{"invalid_input",
                                              "an order payment needs its provider, its id and a "
                                              "state of paid or cancelled"});
        }
        return with_tx<CloudOrderPaymentResult>(
            [&](pqxx::work& tx) -> Result<CloudOrderPaymentResult> {
                CloudOrderPaymentResult result;
                const pqxx::result recorded = tx.exec_params(
                    "INSERT INTO billing_payment_events (provider, event_id, event_type, "
                    "reference) VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING RETURNING 1",
                    payment.provider,
                    payment.event_id,
                    payment.event_type.substr(0, 128),
                    payment.checkout_session_id.substr(0, 256));
                if (recorded.empty()) {
                    result.duplicate = true;
                    return result;
                }
                if (payment.checkout_session_id.empty()) {
                    return result;
                }
                const pqxx::result rows = tx.exec_params(
                    "SELECT id::text FROM cloud_orders WHERE stripe_checkout_session_id = $1 FOR "
                    "UPDATE",
                    payment.checkout_session_id);
                if (rows.empty()) {
                    return result;
                }
                const std::string id = rows[0][0].as<std::string>();
                auto before = cloud_order_in(tx, id);
                if (!before) {
                    return std::unexpected(before.error());
                }
                result.matched = true;
                if (before->state == "pending_payment") {
                    tx.exec_params("UPDATE cloud_orders SET state = $2, stripe_customer_id = "
                                   "coalesce($3, stripe_customer_id), stripe_subscription_id = "
                                   "coalesce($4, stripe_subscription_id) WHERE id = $1::uuid",
                                   id,
                                   payment.state,
                                   nullable(payment.customer_id.substr(0, 255)),
                                   nullable(payment.subscription_id.substr(0, 255)));
                    cloud_order_event(tx,
                                      id,
                                      "pending_payment",
                                      payment.state,
                                      "stripe",
                                      payment.event_type + " " + payment.event_id +
                                          (payment.detail.empty() ? "" : ": " + payment.detail));
                    result.changed = true;
                }
                auto after = cloud_order_in(tx, id);
                if (!after) {
                    return std::unexpected(after.error());
                }
                result.order = std::move(*after);
                return result;
            });
    }

    Result<std::vector<CloudOrder>> claim_cloud_order_emails(int limit,
                                                             int lease_seconds) override {
        return with_tx<std::vector<CloudOrder>>(
            [&](pqxx::work& tx) -> Result<std::vector<CloudOrder>> {
                const pqxx::result rows = tx.exec_params(
                    std::string("UPDATE cloud_orders SET email_retry_at = now() + make_interval("
                                "secs => $2) WHERE id IN (SELECT id FROM cloud_orders WHERE "
                                "((state IN ('submitted', 'paid', 'failed') AND "
                                "support_emailed_state IS DISTINCT FROM state) OR (state IN "
                                "('submitted', 'paid', 'ready', 'failed') AND "
                                "customer_emailed_state IS DISTINCT FROM state)) AND "
                                "(email_retry_at IS NULL OR email_retry_at <= now()) ORDER BY "
                                "updated_at LIMIT $1 FOR UPDATE SKIP LOCKED) RETURNING ") +
                        cloud_order_columns,
                    std::max(limit, 1),
                    std::max(lease_seconds, 1));
                std::vector<CloudOrder> orders;
                for (const pqxx::row& row : rows) {
                    orders.push_back(cloud_order_row(row));
                }
                return orders;
            });
    }

    Result<void> record_cloud_order_email(const CloudOrderEmailOutcome& outcome) override {
        if (!uuid_text(outcome.id) ||
            (outcome.recipient != "support" && outcome.recipient != "customer")) {
            return std::unexpected(
                StoreError{"invalid_input", "an email outcome needs an order and a recipient"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            if (outcome.delivered) {
                tx.exec_params(
                    std::string("UPDATE cloud_orders SET ") +
                        (outcome.recipient == "support" ? "support_emailed_state"
                                                        : "customer_emailed_state") +
                        " = $2, email_attempts = 0, email_error = '', email_retry_at = NULL WHERE "
                        "id = $1::uuid",
                    outcome.id,
                    outcome.state);
            } else {
                tx.exec_params("UPDATE cloud_orders SET email_attempts = email_attempts + 1, "
                               "email_error = $2, email_retry_at = now() + make_interval(secs => "
                               "$3) WHERE id = $1::uuid",
                               outcome.id,
                               utf8_prefix(outcome.error, 1024),
                               std::max(outcome.retry_seconds, 1));
            }
            return {};
        });
    }

    Result<std::vector<CloudOrderEvent>> unaudited_cloud_order_events(int limit) override {
        return with_tx<std::vector<CloudOrderEvent>>(
            [&](pqxx::work& tx) -> Result<std::vector<CloudOrderEvent>> {
                const pqxx::result rows = tx.exec_params(
                    "SELECT id, order_id::text, to_char(at AT TIME ZONE 'UTC', "
                    "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), coalesce(from_state, ''), to_state, actor, "
                    "detail FROM cloud_order_events WHERE audited_at IS NULL ORDER BY id LIMIT $1",
                    std::max(limit, 1));
                std::vector<CloudOrderEvent> events;
                for (const pqxx::row& row : rows) {
                    events.push_back(CloudOrderEvent{row[0].as<std::int64_t>(),
                                                     row[1].as<std::string>(),
                                                     row[2].as<std::string>(),
                                                     row[3].as<std::string>(),
                                                     row[4].as<std::string>(),
                                                     row[5].as<std::string>(),
                                                     row[6].as<std::string>(),
                                                     {}});
                }
                return events;
            });
    }

    Result<void> mark_cloud_order_events_audited(const std::vector<std::int64_t>& ids) override {
        if (ids.empty()) {
            return {};
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            std::string list = "{";
            for (std::size_t index = 0; index < ids.size(); ++index) {
                list += (index == 0 ? "" : ",") + std::to_string(ids[index]);
            }
            list += "}";
            tx.exec_params("UPDATE cloud_order_events SET audited_at = now() WHERE id = "
                           "ANY($1::bigint[]) AND audited_at IS NULL",
                           list);
            return {};
        });
    }

    Result<IssuedSession> sign_up_password(const NewPasswordUser& in) override {
        if (in.pool_id.empty() || in.email.empty() || in.password.size() < 12) {
            return std::unexpected(StoreError{
                "invalid_input", "pool, email, and a password of 12 characters are required"});
        }
        // Lake's own sign-up no longer needs a document (lake#762, decision 65): without one,
        // the caller minted a managed key, which this account signs with until the person uses
        // their own identity. Application pools still register the person's document.
        const bool managed = in.identity_document.empty() && !in.custodial_taap_id.empty();
        if (managed && (in.pool_id != "lake" || in.custodial_public_key.empty() ||
                        in.custodial_wrapped_seed.empty())) {
            return std::unexpected(StoreError{
                "invalid_input", "a managed key is only for lake's own accounts, and complete"});
        }
        Result<VerifiedIdentityDocument> identity =
            managed ? Result<VerifiedIdentityDocument>{VerifiedIdentityDocument{}}
                    : verify_identity_document(in.identity_document);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        if (managed) {
            identity->identifier = in.custodial_taap_id;
        }
        // A document is public: it becomes this account's identity only with a fresh signature by
        // its key (lake#931).
        Result<std::string> proof_id = std::string();
        if (!managed) {
            proof_id = verify_identity_proof(
                in.pool_id, in.identity_document, identity->identifier, in.identity_proof);
            if (!proof_id) {
                return std::unexpected(proof_id.error());
            }
        }
        const std::string password_hash = identity_crypto::hash_secret(in.password);
        if (password_hash.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not derive password credential"});
        }
        return with_authentication_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            if (in.pool_id != "lake" &&
                tx.exec_params("SELECT 1 FROM auth_applications WHERE pool_id = $1", in.pool_id)
                    .empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "application pool does not exist"});
            }
            // The caller may choose the id, so that an application's workspace records the user
            // before this row exists (lake#353); otherwise PostgreSQL makes one.
            const pqxx::result user_rows = tx.exec_params(
                "INSERT INTO users (id, taap_id, email, display_name, pool_id) "
                "VALUES (COALESCE(NULLIF($5, '')::uuid, gen_random_uuid()), $1, $2, $3, $4) "
                "RETURNING id::text",
                identity->identifier,
                in.email,
                in.display_name,
                in.pool_id,
                in.user_id);
            const std::string user_id = user_rows[0][0].as<std::string>();
            if (managed) {
                tx.exec_params("INSERT INTO custodial_identities (user_id, taap_id, "
                               "public_key, wrapped_seed) VALUES ($1::uuid, $2, $3, $4)",
                               user_id,
                               in.custodial_taap_id,
                               in.custodial_public_key,
                               in.custodial_wrapped_seed);
            } else {
                tx.exec_params("INSERT INTO identity_documents (user_id, document, "
                               "document_hash) VALUES ($1::uuid, to_jsonb($2::text), $3)",
                               user_id,
                               identity->document,
                               identity->hash);
                // After the user's row, so a retried sign-up still answers conflict; a proof spent
                // by an OAuth start is refused here.
                if (identity_proof_used(tx, *proof_id)) {
                    return std::unexpected(
                        StoreError{"unauthorized", "that identity proof was already used"});
                }
                try {
                    tx.exec_params("INSERT INTO identity_taap_assertions "
                                   "(assertion_id, user_id, pool_id) VALUES ($1, $2::uuid, $3)",
                                   *proof_id,
                                   user_id,
                                   in.pool_id);
                } catch (const pqxx::unique_violation&) {
                    return std::unexpected(
                        StoreError{"unauthorized", "that identity proof was already used"});
                }
            }
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, "
                           "subject, secret_hash) "
                           "VALUES ($1::uuid, $2, 'password', lower($3), $4)",
                           user_id,
                           in.pool_id,
                           in.email,
                           password_hash);
            if (in.pool_id == "lake") {
                const auto customer =
                    managed ? provision_personal_customer(tx,
                                                          user_id,
                                                          in.custodial_taap_id,
                                                          "sign_up_password/managed/" + user_id)
                            : provision_personal_customer(tx, user_id, *identity);
                if (!customer) {
                    return std::unexpected(customer.error());
                }
            }
            auto issued = issue_session(tx,
                                        user_id,
                                        in.pool_id,
                                        in.device_name,
                                        "browser",
                                        "",
                                        {"*"},
                                        "",
                                        60 * 60 * 24 * 30);
            if (issued) {
                issued->created_user = true;
            }
            return issued;
        });
    }
    Result<IssuedSession> sign_in_password(const PasswordSignIn& in) override {
        return with_authentication_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result rows =
                tx.exec_params("SELECT user_id::text, secret_hash FROM credentials "
                               "WHERE pool_id = $1 AND kind = 'password' AND subject = lower($2) "
                               "AND revoked_at IS NULL FOR SHARE",
                               in.pool_id,
                               in.email);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "invalid email or password"});
            return authentication_attempt(
                rows[0][0].as<std::string>(), [&]() -> Result<IssuedSession> {
                    if (rows.empty() || !identity_crypto::verify_secret(
                                            rows[0][1].as<std::string>(), in.password)) {
                        return std::unexpected(
                            StoreError{"not_found", "invalid email or password"});
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
        });
    }

    Result<IssuedSession> sign_in_taap(const TaapSignIn& input) override {
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            // A linked own identity signs in to the account it was linked to (lake#762).
            const pqxx::result users = tx.exec_params(
                "SELECT id::text FROM users WHERE pool_id = $1 AND (taap_id = $2 OR id = "
                "(SELECT user_id FROM identity_links WHERE own_taap_id = $2))",
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
    Result<bool> mfa_enrolled(const std::string& user_id, const std::string& pool_id) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            const pqxx::result rows = tx.exec_params(
                "SELECT 1 FROM credentials WHERE user_id = $1::uuid AND pool_id = $2 "
                "AND kind = 'totp' AND revoked_at IS NULL LIMIT 1",
                user_id,
                pool_id.empty() ? std::string("lake") : pool_id);
            return !rows.empty();
        });
    }
    Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                     const std::string& pool_id,
                                     const MfaEnrollmentProof& proof) override {
        const std::string pool = pool_id.empty() ? "lake" : pool_id;
        if (user_id.empty() || proof.session_id.empty() || proof.password.empty()) {
            return std::unexpected(
                StoreError{"unauthorized",
                           "MFA enrollment requires an interactive session and current password"});
        }
        MfaEnrollment enrollment;
        enrollment.secret = identity_crypto::new_totp_secret();
        for (int index = 0; index < 8; ++index) {
            const std::string code = identity_crypto::new_recovery_code();
            if (code.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "could not create MFA recovery codes"});
            }
            enrollment.recovery_codes.push_back(code);
        }
        if (enrollment.secret.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create TOTP secret"});
        }
        return with_authentication_tx<MfaEnrollment>([&](pqxx::work& tx) -> Result<MfaEnrollment> {
            const auto session = tx.exec_params(
                "SELECT 1 FROM sessions JOIN users ON users.id = sessions.user_id "
                "WHERE sessions.id = $1::uuid AND sessions.user_id = $2::uuid "
                "AND sessions.pool_id = $3 AND sessions.kind IN ('browser', 'taap') "
                "AND sessions.scopes = ARRAY['*']::text[] AND sessions.parent_session_id IS NULL "
                "AND sessions.revoked_at IS NULL "
                "AND (sessions.expires_at IS NULL OR sessions.expires_at > clock_timestamp()) "
                "AND users.disabled_at IS NULL AND users.deleted_at IS NULL "
                "FOR SHARE OF sessions, users",
                proof.session_id,
                user_id,
                pool);
            if (session.empty() || !active_session_lineage(tx, proof.session_id, user_id, pool)) {
                return std::unexpected(
                    StoreError{"unauthorized",
                               "MFA enrollment requires an active interactive sign-in session"});
            }
            return authentication_attempt(user_id, [&]() -> Result<MfaEnrollment> {
                const auto password = tx.exec_params(
                    "SELECT secret_hash FROM credentials WHERE user_id = $1::uuid AND pool_id = $2 "
                    "AND kind = 'password' AND revoked_at IS NULL FOR UPDATE",
                    user_id,
                    pool);
                if (password.empty() || !identity_crypto::verify_secret(
                                            password[0][0].as<std::string>(), proof.password)) {
                    return std::unexpected(
                        StoreError{"unauthorized", "MFA enrollment requires the current password"});
                }
                const Result<std::string> encrypted =
                    encrypt_secret(tx, "identity.totp", enrollment.secret);
                if (!encrypted) {
                    return std::unexpected(encrypted.error());
                }
                tx.exec_params(
                    "INSERT INTO credentials (user_id, pool_id, kind, subject, public_data) "
                    "VALUES ($1::uuid, $2, 'totp', $1, jsonb_build_object('encrypted_seed', "
                    "$3::text)) "
                    "ON CONFLICT (pool_id, kind, subject) DO UPDATE SET public_data = "
                    "EXCLUDED.public_data, revoked_at = NULL",
                    user_id,
                    pool,
                    *encrypted);
                tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE user_id = $1::uuid "
                               "AND pool_id = $2 AND kind = 'recovery' AND revoked_at IS NULL",
                               user_id,
                               pool);
                for (const std::string& code : enrollment.recovery_codes) {
                    const std::string hash = identity_crypto::hash_secret(code);
                    if (hash.empty()) {
                        return std::unexpected(
                            StoreError{"store_error", "could not protect MFA recovery codes"});
                    }
                    const auto selector = identity_crypto::recovery_selector(code);
                    if (selector.empty())
                        return std::unexpected(
                            StoreError{"store_error", "could not select recovery code"});
                    tx.exec_params(
                        "INSERT INTO credentials (user_id, pool_id, kind, subject, secret_hash) "
                        "VALUES ($1::uuid, $2, 'recovery', $3, $4)",
                        user_id,
                        pool,
                        selector,
                        hash);
                }
                return enrollment;
            });
        });
    }
    Result<void> verify_mfa_code(const std::string& user_id,
                                 const std::string& pool_id,
                                 const std::string& code) override {
        return with_authentication_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params("SELECT public_data->>'encrypted_seed' FROM credentials "
                               "WHERE user_id = $1::uuid AND pool_id = $2 AND kind = 'totp' "
                               "AND revoked_at IS NULL FOR SHARE",
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
            if (identity_crypto::verify_totp(*seed, code)) {
                return {};
            }
            const pqxx::result recovery_rows =
                tx.exec_params("SELECT secret_hash FROM credentials WHERE user_id = $1::uuid "
                               "AND pool_id = $2 AND kind = 'recovery' AND subject = $3 AND $3 <> "
                               "'' AND revoked_at IS NULL",
                               user_id,
                               pool_id,
                               identity_crypto::recovery_selector(code));
            if (recovery_rows.empty())
                return std::unexpected(
                    StoreError{"unauthorized", "a fresh MFA verification is required"});
            return authentication_attempt(user_id, [&]() -> Result<void> {
                if (!identity_crypto::verify_secret(recovery_rows[0][0].as<std::string>(), code))
                    return std::unexpected(
                        StoreError{"unauthorized", "a fresh MFA verification is required"});
                return {};
            });
        });
    }
    Result<void> disable_mfa(const std::string& user_id,
                             const std::string& pool_id,
                             const std::string& code) override {
        return with_authentication_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows =
                tx.exec_params("SELECT public_data->>'encrypted_seed' FROM credentials "
                               "WHERE user_id = $1::uuid AND pool_id = $2 AND kind = 'totp' "
                               "AND revoked_at IS NULL FOR UPDATE",
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
                                   "AND pool_id = $2 AND kind = 'recovery' AND subject = $3 AND "
                                   "$3 <> '' AND revoked_at IS NULL",
                                   user_id,
                                   pool_id,
                                   identity_crypto::recovery_selector(code));
                if (recovery_rows.empty())
                    return std::unexpected(
                        StoreError{"unauthorized", "a fresh MFA verification is required"});
                const auto checked = authentication_attempt(user_id, [&]() -> Result<void> {
                    if (!identity_crypto::verify_secret(recovery_rows[0][0].as<std::string>(),
                                                        code))
                        return std::unexpected(
                            StoreError{"unauthorized", "a fresh MFA verification is required"});
                    return {};
                });
                if (!checked)
                    return std::unexpected(checked.error());
                verified = true;
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
        return with_authentication_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result password_rows =
                tx.exec_params("SELECT user_id::text, secret_hash FROM credentials "
                               "WHERE pool_id = $1 AND kind = 'password' AND subject = lower($2) "
                               "AND revoked_at IS NULL FOR SHARE",
                               in.pool_id,
                               in.email);
            if (password_rows.empty())
                return std::unexpected(StoreError{"not_found", "invalid email or password"});
            return authentication_attempt(
                password_rows[0][0].as<std::string>(), [&]() -> Result<IssuedSession> {
                    if (password_rows.empty() ||
                        !identity_crypto::verify_secret(password_rows[0][1].as<std::string>(),
                                                        in.password)) {
                        return std::unexpected(
                            StoreError{"not_found", "invalid email or password"});
                    }
                    return sign_in_totp_tx(tx, in, password_rows[0][0].as<std::string>());
                });
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
        if (in.provider == "clerk" && (in.pool_id.empty() || in.pool_id == "lake")) {
            return std::unexpected(
                StoreError{"invalid_input", "Clerk requires its verified template exchange"});
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const std::string subject = in.provider + ":" + in.subject;
            if (in.pool_id != "lake" &&
                tx.exec_params("SELECT 1 FROM auth_applications WHERE pool_id = $1", in.pool_id)
                    .empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "application pool does not exist"});
            }
            pqxx::result rows =
                tx.exec_params("SELECT user_id::text FROM credentials WHERE pool_id = $1 "
                               "AND kind = 'oauth' AND subject = $2 AND revoked_at IS NULL",
                               in.pool_id,
                               subject);
            std::string user_id;
            const bool created = rows.empty();
            if (created) {
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
            } else {
                user_id = rows[0][0].as<std::string>();
            }
            auto issued = issue_session(tx,
                                        user_id,
                                        in.pool_id,
                                        in.device_name,
                                        "browser",
                                        "",
                                        {"*"},
                                        "",
                                        60 * 60 * 24 * 30);
            if (issued) {
                issued->created_user = created;
            }
            return issued;
        });
    }
    Result<ClerkUserProvisioned> clerk_user_tx(pqxx::work& tx, const ClerkIdentity& in) {
        if (in.subject.empty()) {
            return std::unexpected(StoreError{"invalid_input", "a Clerk subject is required"});
        }
        tx.exec_params("INSERT INTO clerk_user_states (subject) VALUES ($1) ON CONFLICT DO NOTHING",
                       in.subject);
        const auto state = tx.exec_params(
            "SELECT suspended, deleted FROM clerk_user_states WHERE subject = $1 FOR UPDATE",
            in.subject);
        const std::string subject = "clerk:" + in.subject;
        ClerkUserProvisioned result;
        pqxx::result rows =
            tx.exec_params("SELECT user_id::text FROM credentials WHERE pool_id = 'lake' "
                           "AND kind = 'oauth' AND subject = $1 AND revoked_at IS NULL",
                           subject);
        if (!rows.empty()) {
            result.user_id = rows[0][0].as<std::string>();
        } else {
            // Clerk verified this address, so an existing lake account with it is the same
            // person: link it rather than create a second account.
            if (!in.email.empty()) {
                rows = tx.exec_params("SELECT id::text FROM users WHERE pool_id = 'lake' "
                                      "AND lower(email) = lower($1)",
                                      in.email);
            }
            if (!rows.empty()) {
                result.user_id = rows[0][0].as<std::string>();
                result.linked = true;
            } else {
                if (in.custodial_taap_id.empty() || in.custodial_public_key.empty() ||
                    in.custodial_wrapped_seed.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "a custodial identity is required to create a user"});
                }
                rows = tx.exec_params("INSERT INTO users (taap_id, email, display_name, "
                                      "pool_id) VALUES ($1, $2, $3, 'lake') RETURNING id::text",
                                      in.custodial_taap_id,
                                      nullable(in.email),
                                      in.display_name);
                result.user_id = rows[0][0].as<std::string>();
                result.created = true;
                tx.exec_params("INSERT INTO custodial_identities (user_id, taap_id, "
                               "public_key, wrapped_seed) VALUES ($1::uuid, $2, $3, $4)",
                               result.user_id,
                               in.custodial_taap_id,
                               in.custodial_public_key,
                               in.custodial_wrapped_seed);
                const auto customer =
                    provision_personal_customer(tx, result.user_id, in.custodial_taap_id, subject);
                if (!customer) {
                    return std::unexpected(customer.error());
                }
            }
            tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, subject) "
                           "VALUES ($1::uuid, 'lake', 'oauth', $2)",
                           result.user_id,
                           subject);
        }
        if (state[0][0].as<bool>() || state[0][1].as<bool>()) {
            revoke_agent_delegations(tx, result.user_id);
            tx.exec_params("UPDATE users SET clerk_suspended = true, "
                           "disabled_at = CASE WHEN $2 THEN coalesce(disabled_at, now()) "
                           "ELSE disabled_at END WHERE id = $1::uuid",
                           result.user_id,
                           state[0][1].as<bool>());
            // A suspended subject may first become linked by this provisioning event.
            tx.exec_params("UPDATE sessions SET revoked_at = coalesce(revoked_at, now()) "
                           "WHERE user_id = $1::uuid",
                           result.user_id);
            tx.exec_params("UPDATE devices SET revoked_at = coalesce(revoked_at, now()) "
                           "WHERE user_id = $1::uuid",
                           result.user_id);
        }
        result.taap_id =
            tx.exec_params("SELECT coalesce(taap_id, '') FROM users WHERE id = $1::uuid",
                           result.user_id)[0][0]
                .as<std::string>();
        return result;
    }
    Result<IssuedSession> sign_in_clerk(const ClerkIdentity& in) override {
        if (in.issued_at <= 0 || in.subject.empty() || in.subject.size() > 256) {
            return std::unexpected(
                StoreError{"invalid_input", "a Clerk subject and token issue time are required"});
        }
        return with_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const auto found = clerk_user_tx(tx, in);
            if (!found)
                return std::unexpected(found.error());
            tx.exec_params("SELECT id FROM users WHERE id = $1::uuid FOR UPDATE", found->user_id);
            const auto state = tx.exec_params(
                "SELECT max(s.revoked_before) FROM clerk_user_states s JOIN credentials c "
                "ON c.subject = 'clerk:' || s.subject WHERE c.user_id = $1::uuid "
                "AND c.pool_id = 'lake' AND c.kind = 'oauth'",
                found->user_id);
            if (in.issued_at <= state[0][0].as<std::int64_t>() / 1000) {
                return std::unexpected(
                    StoreError{"unauthorized", "the Clerk token predates revocation"});
            }
            return issue_session(tx,
                                 found->user_id,
                                 "lake",
                                 in.device_name,
                                 "browser",
                                 "",
                                 {"*"},
                                 "",
                                 60 * 60 * 24 * 30);
        });
    }
    Result<void> revoke_clerk_sessions(const std::string& subject,
                                       std::int64_t revoked_at) override {
        if (subject.empty() || subject.size() > 256 || revoked_at < 0) {
            return std::unexpected(StoreError{
                "invalid_input", "a Clerk subject and revocation timestamp are required"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params(
                "INSERT INTO clerk_user_states (subject) VALUES ($1) ON CONFLICT DO NOTHING",
                subject);
            const auto state = tx.exec_params(
                "SELECT revoked_before FROM clerk_user_states WHERE subject = $1 FOR UPDATE",
                subject);
            if (revoked_at < state[0][0].as<std::int64_t>())
                return {};
            tx.exec_params("UPDATE clerk_user_states SET revoked_before = $2 WHERE subject = $1",
                           subject,
                           revoked_at);
            const auto user_id = clerk_user_id(tx, subject);
            if (user_id) {
                // The user lock also serializes every other issuance path with this revocation.
                tx.exec_params("SELECT id FROM users WHERE id = $1::uuid FOR UPDATE", *user_id);
                tx.exec_params("UPDATE sessions SET revoked_at = coalesce(revoked_at, now()) WHERE "
                               "user_id = $1::uuid",
                               *user_id);
                tx.exec_params("UPDATE devices SET revoked_at = coalesce(revoked_at, now()) WHERE "
                               "user_id = $1::uuid",
                               *user_id);
            }
            return {};
        });
    }
    Result<bool> apply_clerk_user_state(const ClerkUserState& in) override {
        if (in.subject.empty() || in.subject.size() > 256 || in.updated_at < 0) {
            return std::unexpected(
                StoreError{"invalid_input", "a Clerk subject and state timestamp are required"});
        }
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            tx.exec_params(
                "INSERT INTO clerk_user_states (subject) VALUES ($1) ON CONFLICT DO NOTHING",
                in.subject);
            const auto state = tx.exec_params("SELECT updated_at, deleted, suspended FROM "
                                              "clerk_user_states WHERE subject = $1 FOR UPDATE",
                                              in.subject);
            const auto user_id = clerk_user_id(tx, in.subject);
            const std::int64_t latest = state[0][0].as<std::int64_t>();
            if (user_id) {
                // Serialize account-wide suspension with other subjects and all issuance paths.
                tx.exec_params("SELECT id FROM users WHERE id = $1::uuid FOR UPDATE", *user_id);
            }
            // A retry after security state committed must still finish provisioning/profile work.
            if (in.updated_at == latest && state[0][0].as<std::int64_t>() == latest &&
                state[0][1].as<bool>() == in.deleted &&
                state[0][2].as<bool>() == (in.suspended || in.deleted)) {
                return true;
            }
            if (state[0][1].as<bool>() || in.updated_at < latest ||
                (in.updated_at == latest && !in.suspended && !in.deleted)) {
                return false;
            }
            tx.exec_params("UPDATE clerk_user_states SET updated_at = GREATEST(updated_at, $2), "
                           "suspended = $3, deleted = $4 WHERE subject = $1",
                           in.subject,
                           in.updated_at,
                           in.suspended || in.deleted,
                           in.deleted);
            if (in.suspended || in.deleted) {
                tx.exec_params("UPDATE clerk_user_states SET revoked_before = "
                               "GREATEST(revoked_before, $2) WHERE subject = $1",
                               in.subject,
                               in.updated_at);
            }
            if (user_id) {
                tx.exec_params("UPDATE users SET clerk_suspended = EXISTS ("
                               "SELECT 1 FROM clerk_user_states s JOIN credentials c "
                               "ON c.subject = 'clerk:' || s.subject WHERE c.user_id = $1::uuid "
                               "AND c.pool_id = 'lake' AND c.kind = 'oauth' AND s.suspended), "
                               "disabled_at = CASE WHEN $2 THEN coalesce(disabled_at, now()) "
                               "ELSE disabled_at END WHERE id = $1::uuid",
                               *user_id,
                               in.deleted);
                if (in.suspended || in.deleted) {
                    revoke_agent_delegations(tx, *user_id);
                    tx.exec_params("UPDATE sessions SET revoked_at = coalesce(revoked_at, now()) "
                                   "WHERE user_id = $1::uuid",
                                   *user_id);
                    tx.exec_params("UPDATE devices SET revoked_at = coalesce(revoked_at, now()) "
                                   "WHERE user_id = $1::uuid",
                                   *user_id);
                }
            }
            return true;
        });
    }
    Result<ClerkUserProvisioned> provision_clerk_user(const ClerkIdentity& in) override {
        return with_tx<ClerkUserProvisioned>(
            [&](pqxx::work& tx) -> Result<ClerkUserProvisioned> { return clerk_user_tx(tx, in); });
    }
    std::optional<std::string> clerk_user_id(pqxx::work& tx, const std::string& clerk_subject) {
        const pqxx::result rows =
            tx.exec_params("SELECT user_id::text FROM credentials WHERE pool_id = 'lake' "
                           "AND kind = 'oauth' AND subject = $1 AND revoked_at IS NULL",
                           "clerk:" + clerk_subject);
        if (clerk_subject.empty() || rows.empty()) {
            return std::nullopt;
        }
        return rows[0][0].as<std::string>();
    }
    Result<std::string> update_clerk_user(const ClerkIdentity& in) override {
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            const auto user_id = clerk_user_id(tx, in.subject);
            if (!user_id) {
                return std::unexpected(
                    StoreError{"not_found", "no Lake account is linked to this user"});
            }
            if (!in.email.empty() &&
                !tx.exec_params("SELECT 1 FROM users WHERE lower(email) = lower($1) "
                                "AND id <> $2::uuid",
                                in.email,
                                *user_id)
                     .empty()) {
                return std::unexpected(StoreError{"conflict", "another account has this email"});
            }
            tx.exec_params("UPDATE users SET email = COALESCE(NULLIF($2, ''), email), "
                           "display_name = $3 WHERE id = $1::uuid",
                           *user_id,
                           in.email,
                           in.display_name);
            return *user_id;
        });
    }
    Result<std::string> disable_clerk_user(const std::string& clerk_subject) override {
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            const auto user_id = clerk_user_id(tx, clerk_subject);
            if (!user_id) {
                return std::unexpected(
                    StoreError{"not_found", "no Lake account is linked to this user"});
            }
            tx.exec_params("UPDATE users SET disabled_at = coalesce(disabled_at, now()) "
                           "WHERE id = $1::uuid",
                           *user_id);
            revoke_agent_delegations(tx, *user_id);
            tx.exec_params("UPDATE sessions SET revoked_at = now() WHERE user_id = $1::uuid "
                           "AND revoked_at IS NULL",
                           *user_id);
            tx.exec_params("UPDATE devices SET revoked_at = now() WHERE user_id = $1::uuid "
                           "AND revoked_at IS NULL",
                           *user_id);
            return *user_id;
        });
    }
    Result<bool> clerk_webhook_applied(const std::string& delivery_id) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            return !tx.exec_params("SELECT 1 FROM clerk_webhook_deliveries WHERE svix_id = $1",
                                   delivery_id)
                        .empty();
        });
    }
    Result<void> record_clerk_webhook(const std::string& delivery_id,
                                      const std::string& event_type,
                                      const std::string& subject) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("INSERT INTO clerk_webhook_deliveries (svix_id, event_type, "
                           "clerk_subject) VALUES ($1, $2, $3) ON CONFLICT DO NOTHING",
                           delivery_id,
                           event_type,
                           subject);
            return {};
        });
    }
    Result<AccountIdentity> account_identity(const std::string& user_id) override {
        return with_tx<AccountIdentity>([&](pqxx::work& tx) -> Result<AccountIdentity> {
            return account_identity_tx(tx, user_id);
        });
    }
    Result<AccountIdentity> account_identity_tx(pqxx::work& tx, const std::string& user_id) {
        const pqxx::result rows = tx.exec_params(
            "SELECT u.id::text, coalesce(u.taap_id, ''), coalesce(c.public_key, ''), "
            "c.wrapped_seed IS NOT NULL AND c.user_id IS NOT NULL, "
            "coalesce(to_char(c.seed_destroyed_at AT TIME ZONE 'UTC', "
            "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), coalesce(l.own_taap_id, ''), "
            "coalesce(l.own_document_hash, ''), coalesce(to_char(l.linked_at AT TIME ZONE 'UTC', "
            "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), coalesce(l.link_document::text, 'null') "
            "FROM users u LEFT JOIN custodial_identities c ON c.user_id = u.id "
            "LEFT JOIN identity_links l ON l.user_id = u.id WHERE u.id = $1::uuid",
            user_id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "user not found"});
        }
        const auto& row = rows[0];
        AccountIdentity identity;
        identity.user_id = row[0].as<std::string>();
        identity.taap_id = row[1].as<std::string>();
        identity.managed_public_key = row[2].as<std::string>();
        identity.managed_seed_held = row[3].as<bool>();
        identity.managed_seed_destroyed_at = row[4].as<std::string>();
        identity.own_taap_id = row[5].as<std::string>();
        identity.own_document_hash = row[6].as<std::string>();
        identity.linked_at = row[7].as<std::string>();
        identity.link_document = nlohmann::json::parse(row[8].as<std::string>());
        identity.key = identity.managed_seed_held ? "managed" : "own";
        return identity;
    }
    Result<ManagedKey> managed_key(const std::string& user_id) override {
        return with_tx<ManagedKey>([&](pqxx::work& tx) -> Result<ManagedKey> {
            const pqxx::result rows =
                tx.exec_params("SELECT taap_id, public_key, wrapped_seed FROM custodial_identities "
                               "WHERE user_id = $1::uuid AND wrapped_seed IS NOT NULL",
                               user_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "this account holds no managed key"});
            }
            return ManagedKey{user_id,
                              rows[0][0].as<std::string>(),
                              rows[0][1].as<std::string>(),
                              rows[0][2].as<std::string>()};
        });
    }
    Result<void> create_identity_link_challenge(const IdentityLinkChallenge& in) override {
        if (in.challenge.empty() || in.user_id.empty() || in.own_taap_id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "the link challenge is incomplete"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            if (!tx.exec_params("SELECT 1 FROM users WHERE taap_id = $1 AND id <> $2::uuid "
                                "UNION ALL SELECT 1 FROM identity_links WHERE own_taap_id = $1",
                                in.own_taap_id,
                                in.user_id)
                     .empty()) {
                return std::unexpected(
                    StoreError{"conflict", "this identity already belongs to a Lake account"});
            }
            tx.exec_params("INSERT INTO identity_link_challenges (challenge, user_id, "
                           "account_taap_id, own_taap_id, own_document, own_document_hash, "
                           "statement, expires_at) VALUES ($1, $2::uuid, $3, $4, $5, $6, "
                           "$7::jsonb, now() + interval '10 minutes')",
                           in.challenge,
                           in.user_id,
                           in.account_taap_id,
                           in.own_taap_id,
                           in.own_document,
                           in.own_document_hash,
                           in.statement.dump());
            return {};
        });
    }
    Result<IdentityLinkChallenge>
    consume_identity_link_challenge(const std::string& user_id,
                                    const std::string& challenge) override {
        return with_tx<IdentityLinkChallenge>([&](pqxx::work& tx) -> Result<IdentityLinkChallenge> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE identity_link_challenges SET used_at = now() WHERE challenge = $1 "
                "AND user_id = $2::uuid AND used_at IS NULL AND expires_at > now() "
                "RETURNING account_taap_id, own_taap_id, own_document, own_document_hash, "
                "statement::text, to_char(expires_at AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",
                challenge,
                user_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "the link challenge is unknown, used or expired"});
            }
            return IdentityLinkChallenge{challenge,
                                         user_id,
                                         rows[0][0].as<std::string>(),
                                         rows[0][1].as<std::string>(),
                                         rows[0][2].as<std::string>(),
                                         rows[0][3].as<std::string>(),
                                         nlohmann::json::parse(rows[0][4].as<std::string>()),
                                         rows[0][5].as<std::string>()};
        });
    }
    Result<AccountIdentity> link_own_identity(const IdentityLink& in) override {
        return with_tx<AccountIdentity>([&](pqxx::work& tx) -> Result<AccountIdentity> {
            // The managed key is locked for the rest of the transaction, so two links of one
            // account cannot both commit.
            const pqxx::result managed = tx.exec_params(
                "SELECT taap_id FROM custodial_identities WHERE user_id = $1::uuid AND "
                "wrapped_seed IS NOT NULL FOR UPDATE",
                in.user_id);
            if (managed.empty() ||
                !tx.exec_params("SELECT 1 FROM identity_links WHERE user_id = $1::uuid", in.user_id)
                     .empty()) {
                return std::unexpected(
                    StoreError{"conflict", "this account already uses its own identity"});
            }
            if (managed[0][0].as<std::string>() != in.account_taap_id) {
                return std::unexpected(
                    StoreError{"conflict", "the account identity changed during the link"});
            }
            if (!tx.exec_params("SELECT 1 FROM users WHERE taap_id = $1 UNION ALL "
                                "SELECT 1 FROM identity_links WHERE own_taap_id = $1",
                                in.own_taap_id)
                     .empty()) {
                return std::unexpected(
                    StoreError{"conflict", "this identity already belongs to a Lake account"});
            }
            try {
                tx.exec_params("INSERT INTO identity_links (user_id, account_taap_id, "
                               "own_taap_id, own_document, own_document_hash, link_document) "
                               "VALUES ($1::uuid, $2, $3, $4, $5, $6::jsonb)",
                               in.user_id,
                               in.account_taap_id,
                               in.own_taap_id,
                               in.own_document,
                               in.own_document_hash,
                               in.link_document.dump());
            } catch (const pqxx::unique_violation&) {
                return std::unexpected(
                    StoreError{"conflict", "this identity already belongs to a Lake account"});
            }
            // The link and the destruction commit together: the seed is never kept after the
            // rotation and never destroyed without it.
            tx.exec_params("UPDATE custodial_identities SET wrapped_seed = NULL, "
                           "seed_destroyed_at = now() WHERE user_id = $1::uuid",
                           in.user_id);
            return account_identity_tx(tx, in.user_id);
        });
    }
    Result<std::string> account_for_identity(const std::string& taap_id) override {
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            const pqxx::result rows =
                tx.exec_params("SELECT account_taap_id FROM identity_links WHERE own_taap_id = $1 "
                               "UNION ALL SELECT taap_id FROM users WHERE taap_id = $1 LIMIT 1",
                               taap_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no account acts for this identity"});
            }
            return rows[0][0].as<std::string>();
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
            in.code_verifier.empty() || in.nonce.empty() || in.browser_binding.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "OAuth flow material is incomplete"});
        }
        // A first sign-in binds the flow's document to the new account (sign_in_oauth), so the flow
        // takes one only with a fresh signature by its key (lake#931). Enterprise flows
        // (`sso-<id>`) carry a TAAP id to sign_in_sso instead of a document (lake#872).
        std::string proof_id;
        if (!in.identity_document.empty() && !in.provider.starts_with("sso-")) {
            auto proven =
                verify_identity_proof(in.pool_id, in.identity_document, "", in.identity_proof);
            if (!proven) {
                return std::unexpected(proven.error());
            }
            proof_id = std::move(*proven);
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            if (!proof_id.empty()) {
                if (identity_proof_used(tx, proof_id)) {
                    return std::unexpected(
                        StoreError{"unauthorized", "that identity proof was already used"});
                }
                // No user exists yet to record the proof against in identity_taap_assertions, so it
                // is spent as a flow row that can never be consumed: already used, its state hash
                // derived from the proof; real flows hash state with their browser binding.
                try {
                    tx.exec_params(
                        "INSERT INTO identity_oauth_flows (pool_id, provider, state_hash, "
                        "encrypted_code_verifier, nonce, device_name, expires_at, used_at) VALUES "
                        "($1, $2, $3, ''::bytea, '', '', to_timestamp($4), now())",
                        in.pool_id,
                        in.provider,
                        identity_crypto::sha256_hex(proof_id),
                        in.identity_proof.expires_at);
                } catch (const pqxx::unique_violation&) {
                    return std::unexpected(
                        StoreError{"unauthorized", "that identity proof was already used"});
                }
            }
            const Result<std::string> encrypted =
                encrypt_secret(tx, "identity.oauth.flow", in.code_verifier);
            if (!encrypted) {
                return std::unexpected(encrypted.error());
            }
            // The existing lookup digest binds the browser without storing its secret. The
            // length prefix keeps arbitrary callback state and cookie bytes unambiguous.
            const std::string state_hash = identity_crypto::sha256_hex(
                std::to_string(in.state.size()) + ":" + in.state + ":" + in.browser_binding);
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
                                    const std::string& state,
                                    const std::string& browser_binding) override {
        if (state.empty() || browser_binding.empty()) {
            return std::unexpected(StoreError{"unauthorized", "OAuth browser binding is required"});
        }
        return with_tx<OAuthFlow>([&](pqxx::work& tx) -> Result<OAuthFlow> {
            const std::string state_hash = identity_crypto::sha256_hex(
                std::to_string(state.size()) + ":" + state + ":" + browser_binding);
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
            if (!in.parent_session_id.empty() &&
                !active_session_lineage(tx, in.parent_session_id, in.user_id, in.pool_id)) {
                return std::unexpected(StoreError{"unauthorized", "issuing session is not active"});
            }
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
                                 lifetime,
                                 in.parent_session_id);
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
    Result<Principal> app_principal(const std::string& secret) {
        const std::string digest = identity_crypto::sha256_hex(secret);
        if (digest.empty()) {
            return std::unexpected(StoreError{"not_found", "app token is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + web_app_columns +
                                   " FROM apps a WHERE a.phase <> 'Deleted' AND "
                                   "(a.token_hashes_json->>'source' = $1 OR "
                                   "a.token_hashes_json->>'zetta' = $1 OR "
                                   "a.token_hashes_json->>'events' = $1 OR "
                                   "a.token_hashes_json->>'pull' = $1)",
                               digest);
            if (rows.size() != 1) {
                return std::unexpected(StoreError{"not_found", "app token is not active"});
            }
            const WebApp app = web_app_from(rows[0]);
            const auto purpose = web_app_token_purpose(app, digest);
            if (!purpose) {
                return std::unexpected(StoreError{"not_found", "app token is not active"});
            }
            return Principal{"",
                             "app:" + app.id + ":" + *purpose,
                             "app:" + app.id,
                             "",
                             "lake",
                             "app_" + *purpose,
                             web_app_token_scopes(*purpose),
                             std::nullopt,
                             app.id,
                             app.customer_id,
                             app.source_repository,
                             app.source_revision,
                             digest};
        });
    }
    Result<Principal> build_principal(const std::string& secret) {
        const std::string digest = identity_crypto::sha256_hex(secret);
        if (digest.empty()) {
            return std::unexpected(StoreError{"not_found", "build token is not active"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + build_columns +
                                   " FROM builds b WHERE b.desired = 'queued' AND "
                                   "b.phase NOT IN ('Succeeded', 'Failed', 'Cancelled') AND "
                                   "(b.token_hashes_json->>'source' = $1 OR "
                                   "b.token_hashes_json->>'push' = $1 OR "
                                   "b.token_hashes_json->>'events' = $1)",
                               digest);
            if (rows.size() != 1) {
                return std::unexpected(StoreError{"not_found", "build token is not active"});
            }
            const Build build = build_from(rows[0]);
            const auto purpose = build_token_purpose(build, digest);
            if (!purpose) {
                return std::unexpected(StoreError{"not_found", "build token is not active"});
            }
            return Principal{"",
                             "build:" + build.id + ":" + *purpose,
                             "build:" + build.id,
                             "",
                             "lake",
                             "build_" + *purpose,
                             build_token_scopes(*purpose),
                             std::nullopt,
                             build.id,
                             build.customer_id,
                             build.repository,
                             build.revision,
                             digest};
        });
    }
    // Every credential's principal passes accepted_principal (lake#870): no credential acts as
    // the operator, and a person's credential names a TAAP identity.
    Result<Principal> context_for(const std::string& secret) override {
        auto principal = accepted_principal(resolve_principal(secret));
        if (principal && !principal->user_id.empty()) {
            return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
                if (auto active = active_delegator(tx, principal->user_id); !active)
                    return std::unexpected(active.error());
                return *principal;
            });
        }
        return principal;
    }

    Result<Principal> resolve_principal(const std::string& secret) {
        if (auto principal = build_principal(secret)) {
            return principal;
        }
        if (auto principal = app_principal(secret)) {
            return principal;
        }
        if (secret.starts_with(methodology_deployment_credential_prefix)) {
            return methodology_deployment_credential(secret);
        }
        if (secret.starts_with(methodology_run_credential_prefix)) {
            return methodology_run_credential(secret);
        }
        if (secret.starts_with(agent_launch_credential_prefix)) {
            return launch_principal(secret);
        }
        if (secret.starts_with(agent_schedule_credential_prefix)) {
            return schedule_principal(secret);
        }
        if (secret.starts_with(agent_task_credential_prefix)) {
            return task_principal(secret);
        }
        const auto lookup_id = session_lookup_id(secret);
        if (!lookup_id) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const std::string digest = identity_crypto::sha256_hex(secret);
        if (digest.empty()) {
            return std::unexpected(StoreError{"store_error", "could not hash session credential"});
        }
        return with_tx<Principal>([&](pqxx::work& tx) -> Result<Principal> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + qualified_session_columns +
                    ", COALESCE(users.taap_id, ''), secret_hash, "
                    "floor(extract(epoch FROM sessions.expires_at))::bigint, "
                    // Expiry is read rather than filtered, so a caller holding the real secret
                    // of an expired credential is told it expired (lake#450).
                    "(sessions.expires_at IS NOT NULL AND sessions.expires_at <= now()) "
                    "FROM sessions "
                    "LEFT JOIN devices ON devices.id = sessions.device_id "
                    "LEFT JOIN users ON users.id = sessions.user_id "
                    "WHERE sessions.lookup_id = $1::uuid AND sessions.revoked_at IS NULL "
                    // An application user's status lives in the application's workspace
                    // (lake#353): suspension and deletion revoke the pool's sessions, and every
                    // session issued in a pool is checked there before it is returned.
                    "AND (devices.id IS NULL OR devices.revoked_at IS NULL) "
                    // A disabled account's credentials no longer work (lake#765).
                    "AND (users.id IS NULL OR (users.disabled_at IS NULL AND NOT "
                    "users.clerk_suspended))",
                *lookup_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            const auto& row = rows.front();
            const std::string expected = row[14].as<std::string>();
            if (expected.size() != digest.size() ||
                CRYPTO_memcmp(expected.data(), digest.data(), digest.size()) != 0) {
                return std::unexpected(StoreError{"not_found", "session is not active"});
            }
            if (row[16].as<bool>()) {
                return std::unexpected(StoreError{"expired", "session has expired"});
            }
            const Session session = session_of(row);
            if (!session.parent_session_id.empty() &&
                !active_session_lineage(tx, session.id, session.user_id, session.pool_id)) {
                return std::unexpected(StoreError{"unauthorized", "issuing session is not active"});
            }
            tx.exec_params("UPDATE sessions SET last_used_at = now() WHERE id = $1::uuid",
                           session.id);
            return Principal{session.user_id,
                             row[13].as<std::string>(),
                             session.id,
                             session.device_id,
                             session.pool_id,
                             session.kind,
                             session.scopes,
                             row[15].is_null()
                                 ? std::nullopt
                                 : std::optional<std::int64_t>(row[15].as<std::int64_t>())};
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

    Result<void> begin_saml(const SamlRequest& in) override {
        if (in.provider_id.empty() || in.request_id.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "SAML request material is incomplete"});
        }
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result provider = tx.exec_params(
                "SELECT 1 FROM sso_providers WHERE id = $1::uuid AND protocol = 'saml' AND enabled",
                in.provider_id);
            if (provider.empty()) {
                return std::unexpected(StoreError{"not_found", "SAML provider is unavailable"});
            }
            tx.exec("DELETE FROM sso_saml_requests WHERE expires_at < now() - interval '1 day'");
            tx.exec_params("INSERT INTO sso_saml_requests (provider_id, request_id_hash, "
                           "device_name, taap_id, return_to, expires_at) VALUES ($1::uuid, $2, $3, "
                           "$4, $5, now() + interval '10 minutes')",
                           in.provider_id,
                           identity_crypto::sha256_hex(in.request_id),
                           in.device_name,
                           in.taap_id,
                           in.return_to);
            return {};
        });
    }

    Result<SamlRequest> consume_saml(const SamlAnswer& in) override {
        if (in.provider_id.empty() || in.request_id.empty() || in.assertion_id.empty()) {
            return std::unexpected(
                StoreError{"unauthorized", "SAML response answers no outstanding request"});
        }
        return with_tx<SamlRequest>([&](pqxx::work& tx) -> Result<SamlRequest> {
            tx.exec("DELETE FROM sso_saml_assertions WHERE expires_at < now()");
            const pqxx::result recorded = tx.exec_params(
                "INSERT INTO sso_saml_assertions (provider_id, assertion_id_hash, expires_at) "
                "VALUES ($1::uuid, $2, greatest(to_timestamp($3), now())) ON CONFLICT DO NOTHING "
                "RETURNING 1",
                in.provider_id,
                identity_crypto::sha256_hex(in.assertion_id),
                static_cast<long long>(in.assertion_expires_at));
            if (recorded.empty()) {
                return std::unexpected(
                    StoreError{"unauthorized", "SAML assertion was already used"});
            }
            const pqxx::result rows = tx.exec_params(
                "UPDATE sso_saml_requests SET used_at = now() WHERE provider_id = $1::uuid AND "
                "request_id_hash = $2 AND used_at IS NULL AND expires_at > now() RETURNING "
                "device_name, taap_id, return_to",
                in.provider_id,
                identity_crypto::sha256_hex(in.request_id));
            if (rows.empty()) {
                // An error is not committed: the assertion's row goes with the transaction.
                return std::unexpected(
                    StoreError{"unauthorized", "SAML response answers no outstanding request"});
            }
            return SamlRequest{in.provider_id,
                               in.request_id,
                               rows[0][0].as<std::string>(),
                               rows[0][1].as<std::string>(),
                               rows[0][2].as<std::string>()};
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
        if (auto checked = check_principal_id(input.taap_id, "taap_id"); !checked) {
            return std::unexpected(checked.error());
        }
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
        if (auto checked = check_principal_id(input.taap_id, "taap_id"); !checked) {
            return std::unexpected(checked.error());
        }
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

    Result<bool> has_active_agent_certificate(const std::string& customer_id,
                                              const std::string& taap_id,
                                              const std::string& at) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            const auto rows = tx.exec_params(
                "SELECT EXISTS (SELECT 1 FROM pki_certificates "
                "WHERE customer_id = $1::uuid AND subject = $2 AND profile = 'agent' "
                "AND revoked_at IS NULL AND expires_at > $3::timestamptz)",
                customer_id,
                taap_id,
                at);
            return rows[0][0].as<bool>();
        });
    }

    Result<AgentSession> create_agent_session(const NewAgentSession& input) override {
        if (input.id.empty() || input.customer_id.empty() || input.durable_agent_taap_id.empty() ||
            input.dispatcher_taap_id.empty() || input.session_taap_id.empty() ||
            input.public_key.empty() || input.scopes.empty() || input.attestation.empty() ||
            input.transparency_root.empty() || input.issued_at.empty() ||
            input.expires_at.empty() ||
            (input.expires_at == "never" ? input.expires_unix != 0 : input.expires_unix <= 0)) {
            return std::unexpected(StoreError{"invalid_input", "agent session record is invalid"});
        }
        return with_tx<AgentSession>([&](pqxx::work& tx) -> Result<AgentSession> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO agent_sessions "
                            "(id, customer_id, durable_agent_taap_id, dispatcher_taap_id, "
                            "session_taap_id, public_key, scopes, attestation, "
                            "transparency_index, transparency_root, issued_at, expires_at) VALUES "
                            "($1::uuid, $2::uuid, $3, $4, $5, $6, "
                            "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, $9, $10, "
                            "$11::timestamptz, $12::timestamptz) RETURNING ") +
                    agent_session_columns,
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
                input.expires_at == "never" ? std::string("infinity") : input.expires_at);
            return agent_session_of(rows[0]);
        });
    }

    Result<AgentSession> get_agent_session(const std::string& customer_id,
                                           const std::string& id) override {
        if (customer_id.empty() || id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent session id is required"});
        }
        return with_tx<AgentSession>([&](pqxx::work& tx) -> Result<AgentSession> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + agent_session_columns +
                                   " FROM agent_sessions "
                                   "WHERE customer_id = $1::uuid AND id = $2::uuid",
                               customer_id,
                               id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "agent session does not exist"});
            }
            return agent_session_of(rows[0]);
        });
    }

    Result<AgentSession> revoke_agent_session(const std::string& customer_id,
                                              const std::string& id) override {
        if (customer_id.empty() || id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "agent session id is required"});
        }
        return with_tx<AgentSession>([&](pqxx::work& tx) -> Result<AgentSession> {
            const pqxx::result rows = tx.exec_params(
                std::string("UPDATE agent_sessions SET revoked_at = coalesce(revoked_at, now()) "
                            "WHERE customer_id = $1::uuid AND id = $2::uuid RETURNING ") +
                    agent_session_columns,
                customer_id,
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "agent session does not exist"});
            }
            return agent_session_of(rows[0]);
        });
    }

    Result<bool> customer_allows_non_expiring_credentials(const std::string& customer_id) override {
        return with_tx<bool>([&](pqxx::work& tx) -> Result<bool> {
            const pqxx::result rows = tx.exec_params(
                "SELECT allow_non_expiring_credentials FROM customers WHERE id = $1::uuid",
                customer_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "customer does not exist"});
            }
            return rows[0][0].as<bool>();
        });
    }

    Result<void> set_customer_non_expiring_credentials(const std::string& customer_id,
                                                       bool allowed) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result rows = tx.exec_params(
                "UPDATE customers SET allow_non_expiring_credentials = $2 WHERE id = $1::uuid "
                "RETURNING id",
                customer_id,
                allowed);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "customer does not exist"});
            }
            return {};
        });
    }

    // A ceremony and its participants, read in `tx`.
    static Result<PkiRecoveryCeremonyRecord> pki_recovery_ceremony_in(
        pqxx::work& tx, const std::string& customer_id, const std::string& ceremony_id) {
        const pqxx::result rows = tx.exec_params(
            "SELECT id::text, customer_id::text, authority_id::text, threshold, "
            "share_count, recovered_at::text, expires_at::text, created_at::text "
            "FROM pki_recovery_ceremonies WHERE customer_id = $1::uuid AND id = $2::uuid",
            customer_id,
            ceremony_id);
        if (rows.empty()) {
            return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
        }
        PkiRecoveryCeremonyRecord record{
            rows[0][0].as<std::string>(),
            rows[0][1].as<std::string>(),
            rows[0][2].as<std::string>(),
            rows[0][3].as<int>(),
            rows[0][4].as<int>(),
            rows[0][5].is_null() ? std::nullopt
                                 : std::optional<std::string>(rows[0][5].as<std::string>()),
            rows[0][6].as<std::string>(),
            rows[0][7].as<std::string>(),
            {}};
        const pqxx::result participants =
            tx.exec_params("SELECT p.share_index, p.principal_id, p.share_issued_at::text, "
                           "s.share_index IS NOT NULL FROM pki_recovery_participants p "
                           "LEFT JOIN pki_recovery_shares s "
                           "ON s.ceremony_id = p.ceremony_id AND s.share_index = p.share_index "
                           "WHERE p.ceremony_id = $1::uuid ORDER BY p.share_index",
                           ceremony_id);
        for (const auto& row : participants) {
            record.participants.push_back(
                {row[0].as<int>(),
                 row[1].as<std::string>(),
                 row[2].is_null() ? std::nullopt
                                  : std::optional<std::string>(row[2].as<std::string>()),
                 row[3].as<bool>()});
        }
        return record;
    }

    Result<PkiRecoveryCeremonyRecord>
    create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& input) override {
        const std::set<std::string> distinct(input.participants.begin(), input.participants.end());
        if (input.id.empty() || input.customer_id.empty() || input.threshold < 2 ||
            input.share_count < input.threshold || input.share_count > 255 ||
            input.expires_at.empty() ||
            static_cast<int>(input.participants.size()) != input.share_count ||
            distinct.size() != input.participants.size() || distinct.contains("")) {
            return std::unexpected(StoreError{"invalid_input", "recovery ceremony is invalid"});
        }
        return with_tx<PkiRecoveryCeremonyRecord>(
            [&](pqxx::work& tx) -> Result<PkiRecoveryCeremonyRecord> {
                const pqxx::result rows = tx.exec_params(
                    "INSERT INTO pki_recovery_ceremonies "
                    "(id, customer_id, authority_id, threshold, share_count, expires_at) "
                    "SELECT $1::uuid, $2::uuid, id, $3, $4, $5::timestamptz "
                    "FROM pki_authorities WHERE customer_id = $2::uuid RETURNING id",
                    input.id,
                    input.customer_id,
                    input.threshold,
                    input.share_count,
                    input.expires_at);
                if (rows.empty()) {
                    return std::unexpected(
                        StoreError{"not_found", "customer authority does not exist"});
                }
                for (std::size_t index = 0; index < input.participants.size(); ++index) {
                    tx.exec_params("INSERT INTO pki_recovery_participants "
                                   "(ceremony_id, share_index, principal_id) "
                                   "VALUES ($1::uuid, $2, $3)",
                                   input.id,
                                   static_cast<int>(index) + 1,
                                   input.participants[index]);
                }
                return pki_recovery_ceremony_in(tx, input.customer_id, input.id);
            });
    }

    Result<PkiRecoveryCeremonyRecord>
    get_pki_recovery_ceremony(const std::string& customer_id,
                              const std::string& ceremony_id) override {
        return with_tx<PkiRecoveryCeremonyRecord>(
            [&](pqxx::work& tx) -> Result<PkiRecoveryCeremonyRecord> {
                return pki_recovery_ceremony_in(tx, customer_id, ceremony_id);
            });
    }

    Result<int> claim_pki_recovery_share(const std::string& customer_id,
                                         const std::string& ceremony_id,
                                         const std::string& principal) override {
        return with_tx<int>([&](pqxx::work& tx) -> Result<int> {
            // One statement: a share is issued at most once, even to concurrent claims.
            const pqxx::result issued = tx.exec_params(
                "UPDATE pki_recovery_participants p SET share_issued_at = now() "
                "FROM pki_recovery_ceremonies c "
                "WHERE c.id = p.ceremony_id AND c.customer_id = $1::uuid AND c.id = $2::uuid "
                "AND p.principal_id = $3 AND p.share_issued_at IS NULL "
                "AND c.recovered_at IS NULL AND c.expires_at > now() RETURNING p.share_index",
                customer_id,
                ceremony_id,
                principal);
            if (!issued.empty()) {
                return issued[0][0].as<int>();
            }
            const pqxx::result ceremony = tx.exec_params(
                "SELECT recovered_at IS NULL AND expires_at > now() FROM pki_recovery_ceremonies "
                "WHERE customer_id = $1::uuid AND id = $2::uuid",
                customer_id,
                ceremony_id);
            if (ceremony.empty()) {
                return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
            }
            if (!ceremony[0][0].as<bool>()) {
                return std::unexpected(StoreError{"conflict", "recovery ceremony is not active"});
            }
            const pqxx::result participant =
                tx.exec_params("SELECT 1 FROM pki_recovery_participants "
                               "WHERE ceremony_id = $1::uuid AND principal_id = $2",
                               ceremony_id,
                               principal);
            if (participant.empty()) {
                return std::unexpected(
                    StoreError{"forbidden", "the caller is not a participant of this ceremony"});
            }
            return std::unexpected(
                StoreError{"conflict", "this participant's recovery share was already issued"});
        });
    }

    Result<PkiRecoveryCeremonyRecord>
    submit_pki_recovery_share(const std::string& customer_id,
                              const std::string& ceremony_id,
                              const std::string& principal,
                              const PkiRecoveryShareEvidence& evidence) override {
        return with_tx<PkiRecoveryCeremonyRecord>([&](pqxx::work& tx)
                                                      -> Result<PkiRecoveryCeremonyRecord> {
            // The ceremony row is locked, so submissions to one ceremony are serialized.
            const pqxx::result active =
                tx.exec_params("SELECT threshold, recovered_at IS NULL AND expires_at > now() "
                               "FROM pki_recovery_ceremonies "
                               "WHERE customer_id = $1::uuid AND id = $2::uuid FOR UPDATE",
                               customer_id,
                               ceremony_id);
            if (active.empty()) {
                return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
            }
            if (!active[0][1].as<bool>()) {
                return std::unexpected(StoreError{"conflict", "recovery ceremony is not active"});
            }
            const pqxx::result participant = tx.exec_params(
                "SELECT share_index, share_issued_at IS NOT NULL FROM pki_recovery_participants "
                "WHERE ceremony_id = $1::uuid AND principal_id = $2",
                ceremony_id,
                principal);
            if (participant.empty() || participant[0][0].as<int>() != evidence.index) {
                return std::unexpected(
                    StoreError{"forbidden", "the caller is not this share's participant"});
            }
            if (!participant[0][1].as<bool>() || evidence.digest.empty()) {
                return std::unexpected(StoreError{
                    "conflict", "this participant's recovery share cannot be presented"});
            }
            const pqxx::result recorded =
                tx.exec_params("INSERT INTO pki_recovery_shares "
                               "(ceremony_id, share_index, share_digest) "
                               "VALUES ($1::uuid, $2, $3) ON CONFLICT DO NOTHING RETURNING 1",
                               ceremony_id,
                               evidence.index,
                               evidence.digest);
            if (recorded.empty()) {
                return std::unexpected(StoreError{
                    "conflict", "this participant's recovery share cannot be presented"});
            }
            tx.exec_params("UPDATE pki_recovery_ceremonies SET recovered_at = now() "
                           "WHERE id = $1::uuid AND (SELECT count(*) FROM pki_recovery_shares "
                           "WHERE ceremony_id = $1::uuid) >= threshold",
                           ceremony_id);
            return pki_recovery_ceremony_in(tx, customer_id, ceremony_id);
        });
    }

    Result<SecretMountRoute> create_secret_mount_route(const SecretMountRoute& input) override {
        return with_tx<SecretMountRoute>([&](pqxx::work& tx) -> Result<SecretMountRoute> {
            // The owner's policy object: the customer's own (an account's mount, no
            // organization), or an organization's inside the customer's namespace.
            const bool account = input.organization_id.empty();
            const pqxx::result scope =
                account ? tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                                         "'customer' AND external_id = $1",
                                         input.customer_id)
                        : tx.exec_params(
                              "SELECT organization_object.id::text FROM objects AS "
                              "organization_object "
                              "JOIN objects AS customer_object ON customer_object.id = "
                              "organization_object.parent_id AND customer_object.object_type = "
                              "'customer' "
                              "WHERE organization_object.object_type = 'organization' "
                              "AND organization_object.external_id = $1 "
                              "AND customer_object.external_id = $2",
                              input.organization_id,
                              input.customer_id);
            if (scope.empty()) {
                return std::unexpected(
                    account ? StoreError{"invalid_input", "customer_id is unknown"}
                            : StoreError{"invalid_input",
                                         "organization_id is not inside the customer namespace"});
            }
            const pqxx::result workspace = tx.exec_params(
                "SELECT 1 FROM workspaces WHERE id = $1::uuid AND customer_id = $2::uuid "
                "AND desired <> 'deleted'",
                input.workspace_id,
                input.customer_id);
            if (workspace.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "workspace_id must name a live workspace of this account"});
            }
            const pqxx::result rows = tx.exec_params(
                "INSERT INTO secret_mount_routes (mount_id, customer_id, organization_id, "
                "workspace_id) VALUES ($1::uuid, $2::uuid, NULLIF($3, '')::uuid, $4::uuid) "
                "RETURNING created_at::text",
                input.mount_id,
                input.customer_id,
                input.organization_id,
                input.workspace_id);
            const std::string environment_external = secret_mount_environment(input);
            const pqxx::result environment =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('environment', $1, $2::uuid) "
                               "ON CONFLICT (object_type, external_id) DO UPDATE SET external_id = "
                               "EXCLUDED.external_id "
                               "RETURNING id::text, coalesce(parent_id::text, '')",
                               environment_external,
                               scope[0][0].as<std::string>());
            // The owner's environment sits under the owner's own object. One that already holds
            // its id elsewhere was made to claim the owner's future mounts (lake#913): it is never
            // adopted, and the mount is refused until the operator removes it.
            if (environment[0][1].as<std::string>() != scope[0][0].as<std::string>()) {
                return std::unexpected(
                    StoreError{"conflict",
                               "the secret mounts' policy object of this owner belongs to another "
                               "account; ask the operator to remove it"});
            }
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('secret_mount', $1, $2::uuid)",
                           input.mount_id,
                           environment[0][0].as<std::string>());
            SecretMountRoute route = input;
            route.created_at = rows[0][0].as<std::string>();
            return route;
        });
    }

    Result<SecretMountRoute> get_secret_mount_route(const std::string& mount_id) override {
        return with_tx<SecretMountRoute>([&](pqxx::work& tx) -> Result<SecretMountRoute> {
            const pqxx::result rows = tx.exec_params(
                "SELECT mount_id::text, customer_id::text, coalesce(organization_id::text, ''), "
                "workspace_id::text, created_at::text FROM secret_mount_routes "
                "WHERE mount_id = $1::uuid",
                mount_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no secret mount " + mount_id});
            }
            return SecretMountRoute{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(),
                                    rows[0][2].as<std::string>(), rows[0][3].as<std::string>(),
                                    rows[0][4].as<std::string>()};
        });
    }

    // ---- deletions (lake#645) ----

    // Every policy object below (and including) the roots, which `DELETE FROM objects` removes
    // with them through objects.parent_id ON DELETE CASCADE, and the grants on them.
    static constexpr const char* policy_subtree_cte =
        "WITH RECURSIVE subtree AS (SELECT id FROM objects WHERE id = ANY($1::uuid[]) "
        "UNION SELECT o.id FROM objects o JOIN subtree s ON o.parent_id = s.id) ";

    Result<OrganizationDeletion> delete_organization(const std::string& id, bool dry_run) override {
        return with_tx<OrganizationDeletion>([&](pqxx::work& tx) -> Result<OrganizationDeletion> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + organization_columns +
                    " FROM organizations WHERE id = $1::uuid AND deleted_at IS NULL FOR UPDATE",
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no organization " + id});
            }
            OrganizationDeletion plan{.organization = organization_of(rows[0])};
            const pqxx::result customers =
                tx.exec_params("SELECT id::text, name FROM customers WHERE organization_id = "
                               "$1::uuid AND deleted_at IS NULL FOR UPDATE",
                               id);
            if (!customers.empty()) {
                plan.customer_id = customers[0][0].as<std::string>();
                plan.customer_name = customers[0][1].as<std::string>();
                const pqxx::result live = tx.exec_params(
                    std::string("SELECT ") + workspace_columns +
                        " FROM workspaces WHERE customer_id = $1::uuid AND desired <> 'deleted' "
                        "ORDER BY created_at, id",
                    plan.customer_id);
                for (const auto& row : live) {
                    plan.live_workspaces.push_back(workspace_from(row));
                }
            }
            const pqxx::result roots =
                tx.exec_params("SELECT coalesce(array_agg(id::text), '{}') FROM objects WHERE "
                               "(object_type = 'organization' AND external_id = $1) OR "
                               "($2 <> '' AND object_type = 'customer' AND external_id = $2)",
                               id,
                               plan.customer_id);
            const std::string root_ids = roots[0][0].as<std::string>();
            const auto count = [&](const std::string& table) -> std::int64_t {
                return tx
                    .exec_params("SELECT count(*) FROM " + table +
                                     " WHERE organization_id = $1::uuid",
                                 id)[0][0]
                    .as<std::int64_t>();
            };
            const std::string mounts = "(organization_id = $1::uuid OR ($2 <> '' AND "
                                       "customer_id::text = $2))";
            for (const char* table :
                 {"teams", "memberships", "invitations", "sso_providers", "federation_issuers"}) {
                plan.counts[table] = count(table);
            }
            plan.counts["secret_mount_routes"] =
                tx.exec_params("SELECT count(*) FROM secret_mount_routes WHERE " + mounts,
                               id,
                               plan.customer_id)[0][0]
                    .as<std::int64_t>();
            plan.counts["policy_objects"] =
                tx.exec_params(std::string(policy_subtree_cte) + "SELECT count(*) FROM subtree",
                               root_ids)[0][0]
                    .as<std::int64_t>();
            plan.counts["grants"] =
                tx.exec_params(std::string(policy_subtree_cte) +
                                   "SELECT count(*) FROM grants WHERE object_id IN "
                                   "(SELECT id FROM subtree)",
                               root_ids)[0][0]
                    .as<std::int64_t>();
            if (dry_run) {
                return plan;
            }
            if (!plan.live_workspaces.empty()) {
                std::string names;
                for (const Workspace& workspace : plan.live_workspaces) {
                    names += (names.empty() ? "" : ", ") + workspace.name;
                }
                return std::unexpected(StoreError{
                    "conflict",
                    "the organization's account still has workspaces; delete them first: " +
                        names});
            }
            tx.exec_params("DELETE FROM secret_lease_routes WHERE mount_id IN (SELECT mount_id "
                           "FROM secret_mount_routes WHERE " +
                               mounts + ")",
                           id,
                           plan.customer_id);
            tx.exec_params("DELETE FROM secret_mount_routes WHERE " + mounts, id, plan.customer_id);
            for (const char* table : {"invitations",
                                      "memberships",
                                      "teams",
                                      "sso_providers",
                                      "federation_issuers",
                                      "offline_recovery_attempts"}) {
                tx.exec_params(
                    std::string("DELETE FROM ") + table + " WHERE organization_id = $1::uuid", id);
            }
            tx.exec_params("DELETE FROM objects WHERE id = ANY($1::uuid[])", root_ids);
            const pqxx::result deleted =
                tx.exec_params("UPDATE organizations SET deleted_at = now() WHERE id = $1::uuid "
                               "RETURNING deleted_at::text",
                               id);
            if (!plan.customer_id.empty()) {
                tx.exec_params("UPDATE customers SET deleted_at = now() WHERE id = $1::uuid",
                               plan.customer_id);
            }
            plan.deleted_at = deleted[0][0].as<std::string>();
            return plan;
        });
    }

    // ---- account deletion (lake#810) ----

    Result<AccountDeletion> delete_account(const std::string& user_id, bool dry_run) override {
        return with_tx<AccountDeletion>([&](pqxx::work& tx) -> Result<AccountDeletion> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + user_columns +
                    " FROM users WHERE id = $1::uuid AND deleted_at IS NULL FOR UPDATE",
                user_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no account " + user_id});
            }
            AccountDeletion plan{.user = user_of(rows[0])};
            const std::string taap = plan.user.taap_id;
            const auto live_workspaces = [&](const std::string& customer_id) {
                std::vector<Workspace> live;
                const pqxx::result found = tx.exec_params(
                    std::string("SELECT ") + workspace_columns +
                        " FROM workspaces WHERE customer_id = $1::uuid AND desired <> 'deleted' "
                        "ORDER BY created_at, id",
                    customer_id);
                for (const auto& row : found) {
                    live.push_back(workspace_from(row));
                }
                return live;
            };
            const pqxx::result customers =
                tx.exec_params("SELECT id::text, name FROM customers WHERE user_id = $1::uuid AND "
                               "deleted_at IS NULL FOR UPDATE",
                               user_id);
            if (!customers.empty()) {
                plan.customer_id = customers[0][0].as<std::string>();
                plan.customer_name = customers[0][1].as<std::string>();
                plan.live_workspaces = live_workspaces(plan.customer_id);
            }
            // The account's memberships: by its user id, or by its TAAP identity.
            const std::string mine = "((principal_type = 'user' AND principal_id = $1) OR "
                                     "($2 <> '' AND principal_taap_id = $2))";
            const pqxx::result organizations = tx.exec_params(
                std::string("SELECT DISTINCT o.id::text FROM memberships m JOIN organizations o "
                            "ON o.id = m.organization_id WHERE o.deleted_at IS NULL AND ") +
                    "((m.principal_type = 'user' AND m.principal_id = $1) OR ($2 <> '' AND "
                    "m.principal_taap_id = $2)) ORDER BY 1",
                user_id,
                taap);
            for (const auto& organization_row : organizations) {
                const std::string organization_id = organization_row[0].as<std::string>();
                const pqxx::result found = tx.exec_params(
                    std::string("SELECT ") + organization_columns +
                        " FROM organizations WHERE id = $1::uuid FOR UPDATE",
                    organization_id);
                AccountOrganization entry{.organization = organization_of(found[0])};
                const pqxx::result role = tx.exec_params(
                    "SELECT role FROM memberships WHERE organization_id = $3::uuid AND team_id IS "
                    "NULL AND " +
                        mine + " ORDER BY (role = 'owner') DESC LIMIT 1",
                    user_id,
                    taap,
                    organization_id);
                if (!role.empty()) {
                    entry.role = role[0][0].as<std::string>();
                }
                entry.other_owners =
                    tx.exec_params("SELECT count(DISTINCT principal_taap_id) FROM memberships "
                                   "WHERE organization_id = $3::uuid AND team_id IS NULL AND "
                                   "role = 'owner' AND NOT " +
                                       mine,
                                   user_id,
                                   taap,
                                   organization_id)[0][0]
                        .as<std::int64_t>();
                entry.other_members =
                    tx.exec_params("SELECT count(DISTINCT principal_taap_id) FROM memberships "
                                   "WHERE organization_id = $3::uuid AND NOT " +
                                       mine,
                                   user_id,
                                   taap,
                                   organization_id)[0][0]
                        .as<std::int64_t>();
                const pqxx::result space =
                    tx.exec_params("SELECT id::text, name FROM customers WHERE organization_id = "
                                   "$1::uuid AND deleted_at IS NULL",
                                   organization_id);
                if (!space.empty()) {
                    entry.customer_id = space[0][0].as<std::string>();
                    entry.customer_name = space[0][1].as<std::string>();
                    entry.live_workspaces = live_workspaces(entry.customer_id);
                }
                plan.organizations.push_back(std::move(entry));
            }
            const pqxx::result roots = tx.exec_params(
                "SELECT coalesce(array_agg(id::text), '{}') FROM objects WHERE $1 <> '' AND "
                "object_type = 'customer' AND external_id = $1",
                plan.customer_id);
            const std::string root_ids = roots[0][0].as<std::string>();
            const auto count = [&](const std::string& sql) -> std::int64_t {
                return tx.exec_params(sql, user_id, taap, plan.customer_id)[0][0]
                    .as<std::int64_t>();
            };
            const std::string by_user = " WHERE user_id = $1::uuid AND $2 = $2 AND $3 = $3";
            const std::string of_space = " WHERE $1 = $1 AND $2 = $2 AND customer_id::text = $3";
            for (const char* table : {"sessions",
                                      "devices",
                                      "credentials",
                                      "identity_documents",
                                      "identity_taap_assertions",
                                      "identity_webauthn_challenges",
                                      "identity_link_challenges",
                                      "identity_links",
                                      "offline_recovery_attempts",
                                      "offline_recovery_grants",
                                      "app_authorization_codes",
                                      "scim_users"}) {
                plan.counts[table] = count(std::string("SELECT count(*) FROM ") + table + by_user);
            }
            plan.counts["custodial_seeds"] =
                count("SELECT count(*) FROM custodial_identities" + by_user +
                      " AND wrapped_seed IS NOT NULL");
            plan.counts["recovery_custody"] =
                count("SELECT count(*) FROM offline_recovery_contacts WHERE $1 = $1 AND $2 <> '' "
                      "AND contact_taap_id = $2 AND $3 = $3");
            plan.counts["memberships"] =
                count("SELECT count(*) FROM memberships WHERE $3 = $3 AND " + mine);
            plan.counts["grants"] =
                count("SELECT count(*) FROM grants WHERE $1 = $1 AND $3 = $3 AND $2 <> '' AND "
                      "principal_taap_id = $2");
            for (const char* table :
                 {"billing_contacts", "customer_usage_limits", "customer_storage_quotas"}) {
                plan.counts[table] = count(std::string("SELECT count(*) FROM ") + table + of_space);
            }
            plan.counts["secret_mount_routes"] =
                count("SELECT count(*) FROM secret_mount_routes" + of_space);
            plan.counts["policy_objects"] =
                tx.exec_params(std::string(policy_subtree_cte) + "SELECT count(*) FROM subtree",
                               root_ids)[0][0]
                    .as<std::int64_t>();
            plan.counts["space_grants"] =
                tx.exec_params(std::string(policy_subtree_cte) +
                                   "SELECT count(*) FROM grants WHERE object_id IN "
                                   "(SELECT id FROM subtree)",
                               root_ids)[0][0]
                    .as<std::int64_t>();
            plan.invoices_kept = count("SELECT count(*) FROM billing_invoices" + of_space);
            plan.open_invoices =
                count("SELECT count(*) FROM billing_invoices" + of_space + " AND state = 'due'");
            if (dry_run) {
                return plan;
            }
            if (!plan.live_workspaces.empty()) {
                std::string names;
                for (const Workspace& workspace : plan.live_workspaces) {
                    names += (names.empty() ? "" : ", ") + workspace.name;
                }
                return std::unexpected(StoreError{
                    "conflict", "the account still has workspaces; delete them first: " + names});
            }
            for (const AccountOrganization& organization : plan.organizations) {
                if (organization.sole_owner()) {
                    return std::unexpected(StoreError{
                        "conflict",
                        "the account is the only owner of the organization " +
                            organization.organization.display_name +
                            "; make another member an owner or delete the organization first"});
                }
            }
            // Identity: every way to sign in or act as the account.
            tx.exec_params("DELETE FROM memberships WHERE $3 = $3 AND " + mine,
                           user_id,
                           taap,
                           plan.customer_id);
            if (!taap.empty()) {
                tx.exec_params("DELETE FROM grants WHERE principal_taap_id = $1", taap);
                tx.exec_params("DELETE FROM offline_recovery_contacts WHERE contact_taap_id = $1",
                               taap);
            }
            for (const char* table : {"scim_users",
                                      "offline_recovery_attempts",
                                      "offline_recovery_grants",
                                      "offline_recovery_notifications",
                                      "app_authorization_codes",
                                      "identity_link_challenges",
                                      "identity_links",
                                      "identity_webauthn_challenges",
                                      "identity_taap_assertions",
                                      "identity_documents",
                                      "sessions",
                                      "credentials",
                                      "devices"}) {
                tx.exec_params(std::string("DELETE FROM ") + table + " WHERE user_id = $1::uuid",
                               user_id);
            }
            tx.exec_params("UPDATE custodial_identities SET wrapped_seed = NULL, "
                           "seed_destroyed_at = coalesce(seed_destroyed_at, now()) "
                           "WHERE user_id = $1::uuid",
                           user_id);
            // The personal space: what it holds outside its (already deleted) workspaces.
            if (!plan.customer_id.empty()) {
                tx.exec_params("DELETE FROM secret_lease_routes WHERE mount_id IN (SELECT "
                               "mount_id FROM secret_mount_routes WHERE customer_id = $1::uuid)",
                               plan.customer_id);
                for (const char* table : {"secret_mount_routes",
                                          "billing_contacts",
                                          "customer_usage_limits",
                                          "customer_storage_quotas"}) {
                    tx.exec_params(std::string("DELETE FROM ") + table +
                                       " WHERE customer_id = $1::uuid",
                                   plan.customer_id);
                }
                tx.exec_params("DELETE FROM objects WHERE id = ANY($1::uuid[])", root_ids);
                tx.exec_params("UPDATE customers SET deleted_at = now() WHERE id = $1::uuid",
                               plan.customer_id);
            }
            const pqxx::result deleted = tx.exec_params(
                "UPDATE users SET email = NULL, display_name = '', deleted_at = now(), "
                "disabled_at = coalesce(disabled_at, now()) WHERE id = $1::uuid "
                "RETURNING deleted_at::text",
                user_id);
            revoke_agent_delegations(tx, user_id);
            plan.deleted_at = deleted[0][0].as<std::string>();
            return plan;
        });
    }

    Result<Invitation> revoke_invitation(const std::string& id, bool dry_run) override {
        return with_tx<Invitation>([&](pqxx::work& tx) -> Result<Invitation> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + invitation_columns +
                    ", (expires_at <= now()) FROM invitations WHERE id = $1::uuid FOR UPDATE",
                id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no invitation " + id});
            }
            Invitation invitation = invitation_of(rows[0]);
            const bool expired = rows[0][12].as<bool>();
            if (invitation.status != "pending" || expired) {
                return std::unexpected(
                    StoreError{"conflict",
                               "only a pending invitation can be revoked; this one is " +
                                   (invitation.status == "pending" ? std::string("expired")
                                                                   : invitation.status)});
            }
            if (dry_run) {
                return invitation;
            }
            const pqxx::result revoked = tx.exec_params(
                std::string("UPDATE invitations SET status = 'revoked' WHERE id = $1::uuid "
                            "RETURNING ") +
                    invitation_columns,
                id);
            return invitation_of(revoked[0]);
        });
    }

    Result<Grant> get_grant(const std::string& id) override {
        return with_tx<Grant>([&](pqxx::work& tx) -> Result<Grant> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + grant_columns + " FROM grants WHERE id = $1::uuid", id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no grant " + id});
            }
            return grant_of(rows[0]);
        });
    }

    Result<Grant> revoke_grant(const std::string& id, bool dry_run) override {
        return with_tx<Grant>([&](pqxx::work& tx) -> Result<Grant> {
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + grant_columns +
                                                         " FROM grants WHERE id = $1::uuid FOR "
                                                         "UPDATE",
                                                     id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no grant " + id});
            }
            const Grant grant = grant_of(rows[0]);
            const pqxx::result derived = tx.exec_params(
                "SELECT EXISTS (SELECT 1 FROM memberships WHERE principal_taap_id = $1 "
                "AND attestation_digest = $2)",
                grant.principal_taap_id,
                grant.attestation_digest);
            if (derived[0][0].as<bool>()) {
                return std::unexpected(StoreError{
                    "conflict", "this grant comes with a membership; remove the member instead"});
            }
            const pqxx::result last = tx.exec_params(
                "SELECT o.object_type = 'customer' AND $2 = 'admin' AND (SELECT count(*) FROM "
                "grants WHERE object_id = o.id AND role = 'admin') = 1 FROM objects o "
                "WHERE o.id = $1::uuid",
                grant.object_id,
                grant.role);
            if (!last.empty() && last[0][0].as<bool>()) {
                return std::unexpected(StoreError{
                    "conflict",
                    "this is the account's last admin grant; grant admin to someone else first"});
            }
            if (!dry_run) {
                tx.exec_params("DELETE FROM grants WHERE id = $1::uuid", id);
            }
            return grant;
        });
    }

    Result<void> delete_secret_mount_route(const std::string& mount_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result removed = tx.exec_params(
                "DELETE FROM secret_mount_routes WHERE mount_id = $1::uuid RETURNING mount_id",
                mount_id);
            if (removed.empty()) {
                return std::unexpected(StoreError{"not_found", "no secret mount " + mount_id});
            }
            tx.exec_params("DELETE FROM secret_lease_routes WHERE mount_id = $1::uuid", mount_id);
            tx.exec_params("DELETE FROM objects WHERE object_type = 'secret_mount' AND "
                           "external_id = $1",
                           mount_id);
            return {};
        });
    }

    Result<std::vector<SecretMountRoute>>
    list_secret_mount_routes(const std::string& customer_id,
                             const std::string& workspace_id) override {
        return with_tx<std::vector<SecretMountRoute>>([&](pqxx::work& tx)
                                                          -> Result<std::vector<SecretMountRoute>> {
            const pqxx::result rows = tx.exec_params(
                "SELECT mount_id::text, customer_id::text, coalesce(organization_id::text, ''), "
                "workspace_id::text, created_at::text FROM secret_mount_routes "
                "WHERE customer_id = $1::uuid AND (NULLIF($2, '') IS NULL OR "
                "workspace_id = NULLIF($2, '')::uuid) ORDER BY created_at DESC, mount_id",
                customer_id,
                workspace_id);
            std::vector<SecretMountRoute> routes;
            for (const pqxx::row& row : rows) {
                routes.push_back({row[0].as<std::string>(),
                                  row[1].as<std::string>(),
                                  row[2].as<std::string>(),
                                  row[3].as<std::string>(),
                                  row[4].as<std::string>()});
            }
            return routes;
        });
    }

    Result<WrappedDataKey> wrap_secret_data_key(const std::string& customer_id,
                                                const std::string& owner_id,
                                                std::uint64_t key_version,
                                                const std::string& raw_key,
                                                const std::string& instance_kek) override {
        return with_tx<WrappedDataKey>([&](pqxx::work& tx) -> Result<WrappedDataKey> {
            auto customer_key = customer_kek_tx(tx, customer_id, instance_kek, true);
            if (!customer_key) {
                return std::unexpected(customer_key.error());
            }
            const pqxx::result keystore = tx.exec_params(
                "SELECT version FROM keystore WHERE customer_id = $1::uuid", customer_id);
            const std::string associated = "lake data key/" + customer_id + "/" + owner_id + "/" +
                                           std::to_string(key_version);
            auto wrapped = secrets_crypto::seal(*customer_key, raw_key, associated);
            secrets_crypto::cleanse(*customer_key);
            if (!wrapped) {
                return std::unexpected(wrapped.error());
            }
            return WrappedDataKey{keystore[0][0].as<std::uint64_t>(), *wrapped};
        });
    }

    Result<std::string> unwrap_secret_data_key(const std::string& customer_id,
                                               const std::string& owner_id,
                                               std::uint64_t key_version,
                                               const WrappedDataKey& wrapped,
                                               const std::string& instance_kek) override {
        return with_tx<std::string>([&](pqxx::work& tx) -> Result<std::string> {
            auto customer_key = customer_kek_version_tx(tx, customer_id, instance_kek,
                                                        wrapped.kek_version);
            if (!customer_key) {
                return std::unexpected(customer_key.error());
            }
            const std::string associated = "lake data key/" + customer_id + "/" + owner_id + "/" +
                                           std::to_string(key_version);
            auto opened = secrets_crypto::open(*customer_key, wrapped.wrapped_key, associated);
            secrets_crypto::cleanse(*customer_key);
            if (!opened || opened->size() != secrets_crypto::Key{}.size()) {
                return std::unexpected(opened ? StoreError{"store_error", "data key is invalid"}
                                              : opened.error());
            }
            return std::move(*opened);
        });
    }

    Result<CustomerKekState> customer_kek_state(const std::string& customer_id) override {
        return with_tx<CustomerKekState>([&](pqxx::work& tx) -> Result<CustomerKekState> {
            const pqxx::result rows = tx.exec_params(
                "SELECT version, COALESCE(previous_version, 0) FROM keystore "
                "WHERE customer_id = $1::uuid",
                customer_id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer key exists"});
            }
            return CustomerKekState{rows[0][0].as<std::uint64_t>(),
                                    rows[0][1].as<std::uint64_t>()};
        });
    }

    Result<void> retire_customer_kek(const std::string& customer_id,
                                     std::uint64_t previous_version) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("UPDATE keystore SET previous_wrapped_key = NULL, previous_version = "
                           "NULL WHERE customer_id = $1::uuid AND previous_version = $2",
                           customer_id,
                           previous_version);
            return {};
        });
    }

    static constexpr std::string_view secret_lease_route_columns =
        "lease_id::text, mount_id::text, customer_id::text, workspace_id::text, "
        "COALESCE((extract(epoch FROM due_at) * 1000000)::bigint, 0), "
        "(extract(epoch FROM created_at) * 1000000)::bigint";

    static SecretLeaseRoute secret_lease_route_row(const pqxx::row& row) {
        return SecretLeaseRoute{row[0].as<std::string>(), row[1].as<std::string>(),
                                row[2].as<std::string>(), row[3].as<std::string>(),
                                row[4].as<std::int64_t>(), row[5].as<std::int64_t>()};
    }

    Result<SecretLeaseRoute> record_secret_lease_route(const SecretLeaseRoute& input) override {
        return with_tx<SecretLeaseRoute>([&](pqxx::work& tx) -> Result<SecretLeaseRoute> {
            const pqxx::result mount = tx.exec_params(
                "SELECT 1 FROM secret_mount_routes WHERE mount_id = $1::uuid "
                "AND workspace_id = $2::uuid AND customer_id = $3::uuid",
                input.mount_id,
                input.workspace_id,
                input.customer_id);
            if (mount.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "a lease's route must name its mount's workspace"});
            }
            tx.exec_params(
                "INSERT INTO secret_lease_routes (lease_id, mount_id, customer_id, workspace_id, "
                "due_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, CASE WHEN $5::bigint = "
                "0 THEN NULL ELSE to_timestamp($5::double precision / 1000000) END) "
                "ON CONFLICT (lease_id) DO NOTHING",
                input.lease_id,
                input.mount_id,
                input.customer_id,
                input.workspace_id,
                input.due_us);
            const pqxx::result rows =
                tx.exec_params("SELECT " + std::string(secret_lease_route_columns) +
                                   " FROM secret_lease_routes WHERE lease_id = $1::uuid",
                               input.lease_id);
            return secret_lease_route_row(rows[0]);
        });
    }

    Result<SecretLeaseRoute> get_secret_lease_route(const std::string& lease_id) override {
        return with_tx<SecretLeaseRoute>([&](pqxx::work& tx) -> Result<SecretLeaseRoute> {
            const pqxx::result rows =
                tx.exec_params("SELECT " + std::string(secret_lease_route_columns) +
                                   " FROM secret_lease_routes WHERE lease_id = $1::uuid",
                               lease_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "dynamic-secret lease does not exist"});
            }
            return secret_lease_route_row(rows[0]);
        });
    }

    Result<void> reschedule_secret_lease(const std::string& lease_id,
                                         std::int64_t due_us) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("UPDATE secret_lease_routes SET due_at = CASE WHEN $2::bigint <= 0 "
                           "THEN NULL ELSE to_timestamp($2::double precision / 1000000) END "
                           "WHERE lease_id = $1::uuid AND due_at IS NOT NULL",
                           lease_id,
                           due_us);
            return {};
        });
    }

    Result<void> delete_secret_lease_route(const std::string& lease_id) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec_params("DELETE FROM secret_lease_routes WHERE lease_id = $1::uuid", lease_id);
            return {};
        });
    }

    // One statement claims the due routes: rows another loop holds are skipped, not waited for.
    Result<std::vector<SecretLeaseRoute>> claim_due_secret_leases(int limit,
                                                                  int hold_seconds) override {
        return with_tx<std::vector<SecretLeaseRoute>>(
            [&](pqxx::work& tx) -> Result<std::vector<SecretLeaseRoute>> {
                const pqxx::result rows = tx.exec_params(
                    "WITH due AS (SELECT lease_id, due_at FROM secret_lease_routes "
                    "WHERE due_at <= now() ORDER BY due_at LIMIT $1 FOR UPDATE SKIP LOCKED) "
                    "UPDATE secret_lease_routes r SET due_at = now() + make_interval(secs => $2) "
                    "FROM due WHERE r.lease_id = due.lease_id RETURNING r.lease_id::text, "
                    "r.mount_id::text, r.customer_id::text, r.workspace_id::text, "
                    "(extract(epoch FROM due.due_at) * 1000000)::bigint, "
                    "(extract(epoch FROM r.created_at) * 1000000)::bigint",
                    std::clamp(limit, 1, 5000),
                    std::max(hold_seconds, 1));
                std::vector<SecretLeaseRoute> claimed;
                for (const pqxx::row& row : rows) {
                    claimed.push_back(secret_lease_route_row(row));
                }
                std::ranges::sort(claimed, {}, &SecretLeaseRoute::due_us);
                return claimed;
            });
    }

    Result<CustomerKekRotation> rotate_customer_kek(const std::string& customer_id,
                                                    const std::string& instance_kek) override {
        return with_tx<CustomerKekRotation>([&](pqxx::work& tx) -> Result<CustomerKekRotation> {
            const pqxx::result keystore =
                tx.exec_params("SELECT version, previous_version IS NOT NULL FROM keystore "
                               "WHERE customer_id = $1::uuid FOR UPDATE",
                               customer_id);
            if (keystore.empty()) {
                return std::unexpected(StoreError{"not_found", "no customer key exists"});
            }
            if (keystore[0][1].as<bool>()) {
                return std::unexpected(StoreError{
                    "conflict", "an earlier customer KEK rotation is still rewrapping data keys"});
            }
            auto root = secrets_crypto::instance_key(instance_kek);
            auto old_key = customer_kek_tx(tx, customer_id, instance_kek, false);
            if (!root || !old_key) {
                return std::unexpected(!root ? root.error() : old_key.error());
            }
            const std::uint64_t previous = keystore[0][0].as<std::uint64_t>();
            const std::uint64_t version = previous + 1;
            auto new_key = secrets_crypto::random_key();
            if (!new_key) {
                secrets_crypto::cleanse(*root);
                secrets_crypto::cleanse(*old_key);
                return std::unexpected(new_key.error());
            }
            const pqxx::result authorities =
                tx.exec_params("SELECT id::text, root_key_ciphertext, intermediate_ciphertext "
                               "FROM pki_authorities WHERE customer_id = $1::uuid FOR UPDATE",
                               customer_id);
            for (const pqxx::row& row : authorities) {
                const std::string root_associated = "lake pki authority/" + customer_id + "/root";
                const std::string intermediate_associated =
                    "lake pki authority/" + customer_id + "/intermediate";
                auto root_key =
                    secrets_crypto::open(*old_key, row[1].as<std::string>(), root_associated);
                auto intermediate = secrets_crypto::open(
                    *old_key, row[2].as<std::string>(), intermediate_associated);
                if (!root_key || !intermediate) {
                    secrets_crypto::cleanse(*root);
                    secrets_crypto::cleanse(*old_key);
                    secrets_crypto::cleanse(*new_key);
                    return std::unexpected(!root_key ? root_key.error() : intermediate.error());
                }
                auto wrapped_root = secrets_crypto::seal(*new_key, *root_key, root_associated);
                auto wrapped_intermediate =
                    secrets_crypto::seal(*new_key, *intermediate, intermediate_associated);
                if (!wrapped_root || !wrapped_intermediate) {
                    secrets_crypto::cleanse(*root);
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
            // The replaced KEK is kept, still wrapped under its own version's associated data,
            // until every workspace data key it wraps has been rewrapped (retire_customer_kek).
            tx.exec_params("UPDATE keystore SET previous_wrapped_key = wrapped_key, "
                           "previous_version = version, wrapped_key = convert_to($2, 'UTF8'), "
                           "version = $3, updated_at = now() WHERE customer_id = $1::uuid",
                           customer_id,
                           *wrapped_customer,
                           version);
            return CustomerKekRotation{
                customer_id,
                version,
                static_cast<std::uint64_t>(authorities.size()),
                previous};
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
        return with_authentication_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
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
            return authentication_attempt(user_id, [&]() -> Result<IssuedSession> {
                const pqxx::result recovery_rows = tx.exec_params(
                    "SELECT id::text, secret_hash FROM credentials WHERE user_id = $1::uuid "
                    "AND pool_id = $2 AND kind = 'recovery' AND subject = $3 AND $3 <> '' AND "
                    "revoked_at IS NULL "
                    "FOR UPDATE",
                    user_id,
                    pool_id,
                    identity_crypto::recovery_selector(code));
                if (recovery_rows.empty() ||
                    !identity_crypto::verify_secret(recovery_rows[0][1].as<std::string>(), code))
                    return std::unexpected(
                        StoreError{"unauthorized", "invalid email or recovery code"});
                tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE id = $1::uuid",
                               recovery_rows[0][0].as<std::string>());
                return issue_session(
                    tx, user_id, pool_id, device_name, "browser", "", {"*"}, "", 86400);
            });
        });
    }

    Result<RecoveryAttempt> begin_email_recovery(const std::string& pool_id,
                                                 const std::string& email,
                                                 int cooling_seconds,
                                                 const std::string& client_address) override {
        if (cooling_seconds < 3600) {
            return std::unexpected(
                StoreError{"invalid_input", "email recovery cooling period is too short"});
        }
        const std::string client_hash = identity_crypto::sha256_hex(client_address);
        if (client_hash.empty()) {
            return std::unexpected(StoreError{"store_error", "could not hash recovery client"});
        }
        // The token is hashed before the transaction (lake#969), so no budget or user row stays
        // locked while Argon2id runs.
        const std::string token = identity_crypto::random_secret();
        const std::string token_hash = identity_crypto::hash_secret(token);
        if (token.empty() || token_hash.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create recovery token"});
        }
        return with_authentication_tx<RecoveryAttempt>([&](pqxx::work& tx)
                                                           -> Result<RecoveryAttempt> {
            // Atomic upserts serialize budgets across replicas. Spend before account lookup,
            // and commit even unknown/throttled requests, so they cannot evade the client cap.
            // The client's budget is spent first and the global budget only for a request the
            // client's budget admits, so one client cannot spend the instance's budget alone.
            const auto spend = [&](const std::string& key, int limit) {
                return !tx.exec_params(
                              "INSERT INTO offline_recovery_budgets AS budget (id, started_at, "
                              "requests) "
                              "VALUES ($1, clock_timestamp(), 1) ON CONFLICT (id) DO UPDATE SET "
                              "started_at = CASE WHEN budget.started_at <= clock_timestamp() - "
                              "interval "
                              "'1 hour' THEN clock_timestamp() ELSE budget.started_at END, "
                              "requests = CASE WHEN budget.started_at <= clock_timestamp() - "
                              "interval "
                              "'1 hour' THEN 1 ELSE budget.requests + 1 END "
                              "WHERE budget.started_at <= clock_timestamp() - interval '1 hour' "
                              "OR budget.requests < $2 RETURNING id",
                              key,
                              limit)
                            .empty();
            };
            // While the global budget is spent, no client row is created, so varying addresses
            // cannot grow the table past the global cap.
            if (!tx.exec("SELECT 1 FROM offline_recovery_budgets WHERE id = 'global' "
                         "AND requests >= 300 "
                         "AND started_at > clock_timestamp() - interval '1 hour'")
                     .empty()) {
                return RecoveryAttempt{};
            }
            tx.exec("DELETE FROM offline_recovery_budgets WHERE id <> 'global' "
                    "AND started_at <= clock_timestamp() - interval '1 hour'");
            if (!spend("client/" + client_hash, 30)) {
                return RecoveryAttempt{};
            }
            if (!spend("global", 300)) {
                return RecoveryAttempt{};
            }
            const pqxx::result users = tx.exec_params(
                "SELECT users.id::text FROM users JOIN credentials ON credentials.user_id = "
                "users.id "
                "WHERE credentials.pool_id = $1 AND credentials.kind = 'password' "
                "AND credentials.subject = lower($2) AND credentials.revoked_at IS NULL "
                "AND users.disabled_at IS NULL AND users.deleted_at IS NULL "
                "AND NOT users.clerk_suspended FOR UPDATE OF users",
                pool_id,
                email);
            if (users.empty()) {
                return RecoveryAttempt{};
            }
            const std::string user_id = users[0][0].as<std::string>();
            // The user row serializes all pools and spellings of this account before any side
            // effect. Reuse does not replace the token, extend its window or enqueue mail.
            const auto pending = tx.exec_params(
                std::string("SELECT ") + recovery_attempt_columns +
                    " FROM offline_recovery_attempts WHERE user_id = $1::uuid AND method = 'email' "
                    "AND status = 'cooling' AND expires_at > clock_timestamp() "
                    "ORDER BY created_at DESC LIMIT 1",
                user_id);
            if (!pending.empty()) {
                return recovery_attempt_of(pending[0]);
            }
            if (!tx.exec_params("SELECT 1 FROM offline_recovery_attempts WHERE user_id = $1::uuid "
                                "AND method = 'email' AND created_at > clock_timestamp() - "
                                "interval '24 hours' LIMIT 1",
                                user_id)
                     .empty()) {
                return RecoveryAttempt{};
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO offline_recovery_attempts ") +
                    "(user_id, method, status, token_hash, ready_at, expires_at, created_at) "
                    "VALUES ($1::uuid, 'email', 'cooling', $2, "
                    "clock_timestamp() + make_interval(secs => $3), "
                    "clock_timestamp() + make_interval(secs => $3) + interval '24 "
                    "hours', clock_timestamp()) RETURNING " +
                    recovery_attempt_columns,
                user_id,
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

    Result<RecoveryAttempt> cancel_recovery(const std::string& user_id,
                                            const std::string& recovery_id) override {
        if (user_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "signed-in account is required"});
        }
        return with_tx<RecoveryAttempt>([&](pqxx::work& tx) -> Result<RecoveryAttempt> {
            // Completion and cancellation lock the same attempt. Whichever commits first wins.
            const auto rows = tx.exec_params(
                std::string("UPDATE offline_recovery_attempts SET status = 'cancelled', ") +
                    "token_hash = NULL WHERE id = $1::uuid AND user_id = $2::uuid "
                    "AND status IN ('cooling', 'pending_approval', 'approved', 'cancelled') "
                    "AND expires_at > clock_timestamp() RETURNING " +
                    recovery_attempt_columns,
                recovery_id,
                user_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "pending recovery attempt not found"});
            }
            tx.exec_params("DELETE FROM offline_recovery_deliveries WHERE recovery_id = $1::uuid "
                           "AND delivered_at IS NULL",
                           recovery_id);
            return recovery_attempt_of(rows[0]);
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
        return with_authentication_tx<IssuedSession>([&](pqxx::work& tx) -> Result<IssuedSession> {
            const pqxx::result attempts = tx.exec_params(
                "SELECT user_id::text, method, status, token_hash, ready_at <= now(), "
                "expires_at > now() FROM offline_recovery_attempts WHERE id = $1::uuid FOR UPDATE",
                recovery_id);
            if (attempts.empty())
                return std::unexpected(StoreError{"unauthorized", "invalid recovery token"});
            return authentication_attempt(
                attempts[0][0].as<std::string>(), [&]() -> Result<IssuedSession> {
                    if (attempts.empty() || attempts[0][3].is_null() ||
                        !identity_crypto::verify_secret(attempts[0][3].as<std::string>(), token)) {
                        return std::unexpected(
                            StoreError{"unauthorized", "invalid recovery token"});
                    }
                    if (!attempts[0][4].as<bool>()) {
                        return std::unexpected(
                            StoreError{"conflict", "recovery cooling period has not ended"});
                    }
                    if (!attempts[0][5].as<bool>()) {
                        return std::unexpected(
                            StoreError{"conflict", "recovery token has expired"});
                    }
                    const std::string method = attempts[0][1].as<std::string>();
                    const std::string status = attempts[0][2].as<std::string>();
                    if ((method == "email" && status != "cooling") ||
                        (method == "admin" && status != "approved")) {
                        return std::unexpected(
                            StoreError{"conflict", "recovery is not completable"});
                    }
                    const std::string user_id = attempts[0][0].as<std::string>();
                    tx.exec_params("UPDATE sessions SET revoked_at = coalesce(revoked_at, now()) "
                                   "WHERE user_id = $1::uuid",
                                   user_id);
                    tx.exec_params("UPDATE offline_recovery_attempts SET status = 'completed', "
                                   "completed_at = now(), token_hash = NULL WHERE id = $1::uuid",
                                   recovery_id);
                    const pqxx::result pool =
                        tx.exec_params("SELECT pool_id FROM credentials WHERE user_id = $1::uuid "
                                       "AND kind = 'password' "
                                       "AND revoked_at IS NULL LIMIT 1",
                                       user_id);
                    if (pool.empty()) {
                        return std::unexpected(
                            StoreError{"not_found", "password credential not found"});
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

    // The customer KEK of one version: the current one or, during a rotation, the previous one.
    Result<secrets_crypto::Key> customer_kek_version_tx(pqxx::work& tx,
                                                        const std::string& customer_id,
                                                        const std::string& instance_kek,
                                                        std::uint64_t version) {
        const pqxx::result rows = tx.exec_params(
            "SELECT CASE WHEN version = $2 THEN convert_from(wrapped_key, 'UTF8') "
            "WHEN previous_version = $2 THEN convert_from(previous_wrapped_key, 'UTF8') END "
            "FROM keystore WHERE customer_id = $1::uuid",
            customer_id,
            version);
        if (rows.empty() || rows[0][0].is_null()) {
            return std::unexpected(StoreError{
                "not_found", "no customer key version " + std::to_string(version) + " exists"});
        }
        auto root = secrets_crypto::instance_key(instance_kek);
        if (!root) {
            return std::unexpected(root.error());
        }
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
        const pqxx::result inserted =
            tx.exec_params("INSERT INTO keystore (purpose, wrapped_key, version, customer_id) "
                           "VALUES ($1, convert_to($2, 'UTF8'), 1, $3::uuid) "
                           "ON CONFLICT (purpose) DO NOTHING RETURNING version",
                           "customer-kek/" + customer_id,
                           *wrapped,
                           customer_id);
        if (inserted.empty()) {
            // Another replica created it first: use that one.
            secrets_crypto::cleanse(*generated);
            return customer_kek_tx(tx, customer_id, instance_kek, false);
        }
        return generated;
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

    Result<std::string> provision_personal_customer(pqxx::work& tx,
                                                    const std::string& user_id,
                                                    const VerifiedIdentityDocument& identity) {
        return provision_personal_customer(
            tx, user_id, identity.identifier, "sign_up_password/" + identity.hash);
    }

    Result<std::string> provision_personal_customer(pqxx::work& tx,
                                                    const std::string& user_id,
                                                    const std::string& taap_id,
                                                    const std::string& attestation_digest) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            const std::string name = generate_handle();
            if (!tx.exec_params("SELECT 1 FROM customers WHERE name = $1", name).empty()) {
                continue;
            }
            const auto customers =
                tx.exec_params("INSERT INTO customers (customer_type, user_id, name) "
                               "VALUES ('individual', $1::uuid, $2) ON CONFLICT (name) DO NOTHING "
                               "RETURNING id::text",
                               user_id,
                               name);
            if (customers.empty()) {
                continue;
            }
            const std::string customer_id = customers[0][0].as<std::string>();
            const auto objects = tx.exec_params(
                "INSERT INTO objects (object_type, external_id) VALUES ('customer', $1) "
                "RETURNING id::text",
                customer_id);
            tx.exec_params(
                "INSERT INTO grants (principal_taap_id, object_id, role, attestation_digest, "
                "granted_by_taap_id) VALUES ($1, $2::uuid, 'admin', $3, $1)",
                taap_id,
                objects[0][0].as<std::string>(),
                attestation_digest);
            return customer_id;
        }
        return std::unexpected(
            StoreError{"conflict", "could not allocate a personal customer handle; retry sign-up"});
    }

    Result<IdentityProofChallenge>
    begin_identity_proof(const std::string& pool_id,
                         const std::string& identity_document) override {
        if (pool_id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "a pool is required"});
        }
        const auto document = own_identity::verify_document(identity_document);
        if (!document) {
            return std::unexpected(document.error());
        }
        const auto key = instance_kek();
        if (!key) {
            return std::unexpected(key.error());
        }
        IdentityProofChallenge issued{"", identity_crypto::random_secret(), 0};
        issued.expires_at = unix_seconds_now() + own_identity::enrollment_lifetime_seconds;
        issued.challenge = own_identity::enrollment_challenge(
            *key, pool_id, *document, issued.nonce, issued.expires_at);
        if (issued.nonce.empty() || issued.challenge.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not issue an identity proof challenge"});
        }
        return issued;
    }

    static std::int64_t unix_seconds_now() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Checks that `proof` is a signature by the key of `identity_document` over the challenge
    // begin_identity_proof issued for it in `pool_id`, still unexpired (lake#931). `taap_id`, when
    // given, must be the document's identity. Answers the proof's one-use id; whether it was spent
    // is the caller's check, in its transaction (identity_proof_used).
    Result<std::string> verify_identity_proof(std::string_view pool_id,
                                              std::string_view identity_document,
                                              std::string_view taap_id,
                                              const IdentityProof& proof) const {
        const auto document = own_identity::verify_document(identity_document);
        if (!document) {
            return std::unexpected(document.error());
        }
        if (proof.nonce.empty() || proof.expires_at == 0 || proof.signature.empty()) {
            return std::unexpected(StoreError{
                "identity_proof_required",
                "Prove you hold this identity's key: sign the challenge begin_identity_proof "
                "issues with liquid sign artifact, and send what it printed as identity_proof."});
        }
        if (!taap_id.empty() && taap_id != document->taap_id) {
            return std::unexpected(
                StoreError{"invalid_input", "the identity proof names another identity"});
        }
        const std::int64_t now = unix_seconds_now();
        if (proof.expires_at <= now ||
            proof.expires_at > now + own_identity::enrollment_lifetime_seconds) {
            return std::unexpected(StoreError{
                "unauthorized", "The identity proof's challenge has expired; ask for a new one."});
        }
        const auto key = instance_kek();
        if (!key) {
            return std::unexpected(key.error());
        }
        const std::string challenge = own_identity::enrollment_challenge(
            *key, pool_id, *document, proof.nonce, proof.expires_at);
        if (challenge.empty()) {
            return std::unexpected(
                StoreError{"store_error", "could not compute the identity proof challenge"});
        }
        const auto signed_value =
            own_identity::verify_challenge_signature(document->encoded, challenge, proof.signature);
        if (!signed_value) {
            return std::unexpected(signed_value.error());
        }
        return "identity_proof:" + challenge;
    }

    // Whether a proof was spent: by a sign-up (identity_taap_assertions) or an OAuth start (its
    // spent row in identity_oauth_flows).
    static bool identity_proof_used(pqxx::work& tx, const std::string& proof_id) {
        return !tx.exec_params("SELECT 1 FROM identity_taap_assertions WHERE assertion_id = $1 "
                               "UNION ALL SELECT 1 FROM identity_oauth_flows WHERE state_hash = $2",
                               proof_id,
                               identity_crypto::sha256_hex(proof_id))
                    .empty();
    }

    Result<VerifiedIdentityDocument> verify_identity_document(std::string_view serialized) const {
        const auto encoded = decode_base64url(serialized);
        if (!encoded) {
            return std::unexpected(
                StoreError{"invalid_input",
                           "TAAP identity document is not base64url. Run liquid identity show "
                           "--document and paste the complete output, without quotes."});
        }
        const auto document = liquid::decode_identity_document(*encoded);
        if (!document) {
            return std::unexpected(StoreError{
                "invalid_input",
                "TAAP identity document is incomplete or invalid canonical CBOR: " +
                    document.error().message +
                    ". Run liquid identity show --document and paste the complete output."});
        }
        const auto verified = liquid::verify_document(*document);
        if (!verified || !*verified) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document signature is invalid"});
        }
        // Only an initial document proves which identity it belongs to: its identifier is its own
        // key's fingerprint. A successor's identifier is proven only by its chain, which this
        // document does not carry (liquid#383).
        if (const auto chain = liquid::verify_chain(std::span{&*document, 1}); !chain) {
            return std::unexpected(StoreError{
                "invalid_input",
                "This TAAP identity has been rotated, so its document alone does not prove which "
                "identity it belongs to. Sign up with the identity's first document."});
        }
        const auto canonical = liquid::encode_identity_document(*document);
        if (!canonical || *canonical != *encoded) {
            return std::unexpected(
                StoreError{"invalid_input", "TAAP identity document is not canonical"});
        }
        // lake#917: the account is bound to the identifier this document's own key derives. A
        // document naming another identifier with a different key verifies in liquid but proves
        // nothing about that identifier, and a rotated successor needs its chain.
        if (!identity_crypto::identity_document_is_self_certifying(*document)) {
            return std::unexpected(StoreError{
                "invalid_input",
                "TAAP identity document must be the identity's first document, whose key derives "
                "its identifier. Register before rotating, or run liquid identity show "
                "--document on the identity you created."});
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
                                        int lifetime_seconds,
                                        const std::string& parent_session_id = {}) {
        const std::string random = identity_crypto::random_secret();
        if (random.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create session secret"});
        }
        // Serialize every issuance path with upstream suspension and local disablement.
        if (auto active = active_delegator(tx, user_id, true); !active)
            return std::unexpected(active.error());
        const std::string lookup_id = new_uuid_v4();
        const std::string secret = "ls1." + lookup_id + "." + random;
        const std::string secret_hash = identity_crypto::sha256_hex(secret);
        if (secret_hash.empty()) {
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
        const pqxx::result sessions = tx.exec_params(
            std::string("INSERT INTO sessions (user_id, device_id, pool_id, kind, "
                        "name, secret_hash, ") +
                "scopes, reason, expires_at, parent_session_id, lookup_id) VALUES ($1::uuid, "
                "$2::uuid, $3, "
                "$4, "
                "$5, $6, "
                "ARRAY(SELECT jsonb_array_elements_text($7::jsonb)), $8, "
                "LEAST(CASE WHEN $9 > 0 THEN now() + make_interval(secs => $9) ELSE NULL "
                "END, (SELECT expires_at FROM sessions WHERE id = $10::uuid)), "
                "$10::uuid, $11::uuid) RETURNING " +
                session_columns,
            user_id,
            nullable(device_id),
            pool_id,
            kind,
            name,
            secret_hash,
            nlohmann::json(scopes).dump(),
            reason,
            lifetime_seconds,
            nullable(parent_session_id),
            lookup_id);
        const Session session = session_of(sessions[0]);
        return IssuedSession{session, secret};
    }

    Result<IssuedSession>
    sign_in_totp_tx(pqxx::work& tx, const PasswordSignIn& in, const std::string& user_id) {
        const pqxx::result rows =
            tx.exec_params("SELECT public_data->>'encrypted_seed' FROM credentials WHERE user_id "
                           "= $1::uuid "
                           "AND pool_id = $2 AND kind = 'totp' AND revoked_at IS NULL FOR SHARE",
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
                "AND pool_id = $2 AND kind = 'recovery' AND subject = $3 AND $3 <> '' AND "
                "revoked_at IS NULL "
                "FOR UPDATE",
                user_id,
                in.pool_id,
                identity_crypto::recovery_selector(in.totp_code));
            if (recovery_rows.empty() || !identity_crypto::verify_secret(
                                             recovery_rows[0][1].as<std::string>(), in.totp_code))
                return std::unexpected(StoreError{"not_found", "invalid TOTP code"});
            tx.exec_params("UPDATE credentials SET revoked_at = now() WHERE id = $1::uuid",
                           recovery_rows[0][0].as<std::string>());
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
    // Hashing must not hold the shared connection mutex: unrelated HTTP/store reads use that
    // connection. Authentication gets a private transaction; row locks preserve one-use codes.
    template <class T, class F> Result<T> with_authentication_tx(F&& body) {
        try {
            pqxx::connection connection(uri_);
            pqxx::work tx(connection);
            Result<T> result = body(tx);
            if (result)
                tx.commit();
            return result;
        } catch (const pqxx::broken_connection& error) {
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
