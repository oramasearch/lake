// SPDX-License-Identifier: MIT

#include "lake/control/apps.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "lake/control/identity_crypto.hpp"

namespace lake::control {
namespace {

std::string nullable_text(const std::string& value) {
    return value;
}

std::string base64url(const unsigned char* bytes, std::size_t size) {
    std::string result(((size + 2) / 3) * 4, '\0');
    const int encoded = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()), bytes, static_cast<int>(size));
    if (encoded < 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(encoded));
    while (result.ends_with('=')) {
        result.pop_back();
    }
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    return result;
}

std::string base64url(std::string_view value) {
    return base64url(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::vector<unsigned char> decode_base64url(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) {
        value.push_back('=');
    }
    std::vector<unsigned char> decoded((value.size() / 4) * 3);
    const int size = EVP_DecodeBlock(decoded.data(),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (value.ends_with('=')) {
        ++padding;
    }
    if (value.ends_with("==")) {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(size) - padding);
    return decoded;
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result += std::format("{:02x}", static_cast<unsigned>(bytes[index]));
    }
    return result;
}

long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace apps {

CryptoResult generate_signing_key(std::string_view application_id, int version) {
    if (application_id.empty() || version < 1) {
        return std::unexpected("application id and positive key version are required");
    }
    std::array<unsigned char, 32> seed{};
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
        return std::unexpected("could not generate application signing key");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
        EVP_PKEY_free);
    if (!key) {
        return std::unexpected("could not create application signing key");
    }
    std::array<unsigned char, 32> public_key{};
    std::size_t public_size = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &public_size) != 1 ||
        public_size != public_key.size()) {
        return std::unexpected("could not derive application public key");
    }
    const std::string kid = "app-" + std::string(application_id) + "-" + std::to_string(version);
    return ApplicationSigningKey{std::string(application_id),
                                 kid,
                                 base64url(seed.data(), seed.size()),
                                 base64url(public_key.data(), public_key.size()),
                                 version,
                                 true,
                                 {},
                                 {}};
}

std::expected<std::string, std::string> issue_jwt(const Application& application,
                                                  const ApplicationSigningKey& key,
                                                  const ApplicationUser& user,
                                                  std::string_view issuer,
                                                  int lifetime_seconds,
                                                  std::string_view nonce) {
    if (key.seed.empty() || !key.current || lifetime_seconds < 1 || lifetime_seconds > 3600) {
        return std::unexpected("a current signing key and a lifetime up to one hour are required");
    }
    const auto decoded_seed = decode_base64url(key.seed);
    if (decoded_seed.size() != 32) {
        return std::unexpected("application signing key is malformed");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> signing_key(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, decoded_seed.data(), decoded_seed.size()),
        EVP_PKEY_free);
    if (!signing_key) {
        return std::unexpected("could not load application signing key");
    }
    const long issued_at = unix_now();
    const nlohmann::json header = {{"alg", "EdDSA"}, {"kid", key.kid}, {"typ", "JWT"}};
    nlohmann::json claims = {{"iss", issuer},
                             {"sub", user.taap_id.empty() ? user.user_id : user.taap_id},
                             {"aud", application.id},
                             {"application_id", application.id},
                             {"pool_id", application.pool_id},
                             {"user_id", user.user_id},
                             {"iat", issued_at},
                             {"exp", issued_at + lifetime_seconds},
                             {"jti", identity_crypto::random_secret()}};
    if (!nonce.empty()) {
        claims["nonce"] = nonce;
    }
    const std::string signing_input = base64url(header.dump()) + "." + base64url(claims.dump());
    std::array<unsigned char, 64> signature{};
    std::size_t signature_size = signature.size();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, signing_key.get()) != 1 ||
        EVP_DigestSign(context.get(),
                       signature.data(),
                       &signature_size,
                       reinterpret_cast<const unsigned char*>(signing_input.data()),
                       signing_input.size()) != 1) {
        return std::unexpected("could not sign application JWT");
    }
    return signing_input + "." + base64url(signature.data(), signature_size);
}

std::string
webhook_signature(std::string_view secret, std::string_view event_id, std::string_view payload) {
    const std::string signed_payload = std::string(event_id) + "." + std::string(payload);
    unsigned int size = EVP_MAX_MD_SIZE;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (HMAC(EVP_sha256(),
             secret.data(),
             static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(signed_payload.data()),
             signed_payload.size(),
             digest.data(),
             &size) == nullptr) {
        return {};
    }
    return "v1=" + hex(digest.data(), size);
}

} // namespace apps

void apply_settings(Application& application, const ApplicationSettings& settings) {
    application.name = settings.name;
    application.allowed_origins = settings.allowed_origins;
    application.redirect_uris = settings.redirect_uris;
    application.theme = settings.theme;
    if (!settings.updated_at.empty()) {
        application.updated_at = settings.updated_at;
    }
}

nlohmann::json to_json(const Application& value) {
    return {{"id", value.id},
            {"customer_id", value.customer_id},
            {"pool_id", value.pool_id},
            {"name", value.name},
            {"allowed_origins", value.allowed_origins},
            {"redirect_uris", value.redirect_uris},
            {"custom_sign_in_domain", value.custom_sign_in_domain},
            {"theme", value.theme},
            {"workspace_id", nullable_text(value.workspace_id)},
            {"created_at", value.created_at},
            {"updated_at", value.updated_at}};
}

nlohmann::json to_json(const ApplicationUser& value) {
    return {{"application_id", value.application_id},
            {"user_id", value.user_id},
            {"taap_id", value.taap_id},
            {"email", value.email},
            {"display_name", value.display_name},
            {"status", value.status},
            {"created_at", value.created_at},
            {"updated_at", value.updated_at}};
}

nlohmann::json to_json(const ApplicationOrganization& value) {
    return {{"id", value.id},
            {"application_id", value.application_id},
            {"taap_id", value.taap_id},
            {"name", value.name},
            {"metadata", value.metadata},
            {"created_at", value.created_at},
            {"updated_at", value.updated_at}};
}

nlohmann::json to_json(const ApplicationWebhook& value) {
    return {{"id", value.id},
            {"application_id", value.application_id},
            {"url", value.url},
            {"events", value.events},
            {"enabled", value.enabled},
            {"created_at", value.created_at},
            {"updated_at", value.updated_at}};
}

nlohmann::json to_json(const ApplicationWebhookEvent& value) {
    return {{"id", value.id},
            {"application_id", value.application_id},
            {"event", value.event},
            {"payload", value.payload},
            {"created_at", value.created_at}};
}

nlohmann::json to_json(const ApplicationWebhookDelivery& value) {
    return {{"id", value.id},
            {"event_id", value.event_id},
            {"webhook_id", value.webhook_id},
            {"attempt", value.attempt},
            {"status", value.status},
            {"signature", value.signature},
            {"error", nullable_text(value.error)},
            {"pending", value.pending},
            {"created_at", value.created_at}};
}

} // namespace lake::control
