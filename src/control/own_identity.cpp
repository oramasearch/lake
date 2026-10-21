#include "lake/control/own_identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <span>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <liquid/hash.hpp>
#include <liquid/identity.hpp>

#include "lake/audit/audit.hpp"

namespace lake::control::own_identity {
namespace {

StoreError invalid(std::string message) {
    return StoreError{"invalid_input", std::move(message)};
}

std::string trimmed(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::optional<std::vector<std::byte>> from_base64url(std::string_view value) {
    std::string text(value);
    for (char& c : text) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        } else if (!std::isalnum(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    if (text.empty() || text.size() % 4 == 1) {
        return std::nullopt;
    }
    const std::size_t padding = (4 - text.size() % 4) % 4;
    text.append(padding, '=');
    std::vector<std::byte> output(text.size() / 4 * 3);
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return std::nullopt;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::optional<std::vector<std::byte>> from_hex(std::string_view value) {
    if (value.empty() || value.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::vector<std::byte> output;
    output.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int high = digit(value[index]);
        const int low = digit(value[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        output.push_back(std::byte{static_cast<unsigned char>(high * 16 + low)});
    }
    return output;
}

std::string hex(std::span<const unsigned char> bytes) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

std::optional<std::string> sha512_256_hex(std::span<const std::byte> bytes) {
    EVP_MD* algorithm = EVP_MD_fetch(nullptr, "SHA512-256", nullptr);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    const bool hashed = algorithm != nullptr && context != nullptr &&
                        EVP_DigestInit_ex(context, algorithm, nullptr) == 1 &&
                        EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
                        EVP_DigestFinal_ex(context, digest.data(), &size) == 1;
    EVP_MD_CTX_free(context);
    EVP_MD_free(algorithm);
    if (!hashed) {
        return std::nullopt;
    }
    return hex(std::span<const unsigned char>(digest.data(), size));
}

std::vector<std::byte> as_bytes(const std::vector<std::uint8_t>& value) {
    std::vector<std::byte> output(value.size());
    std::ranges::transform(value, output.begin(), [](std::uint8_t b) { return std::byte{b}; });
    return output;
}

// The signature hex `liquid sign artifact` printed: a `signature=` line, or the hex alone.
std::string signature_hex(std::string_view output, std::string& signer) {
    std::string found;
    std::size_t start = 0;
    while (start <= output.size()) {
        const std::size_t end = output.find('\n', start);
        const std::string line = trimmed(output.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (line.starts_with("signature=")) {
            found = line.substr(10);
        } else if (line.starts_with("signer=")) {
            signer = line.substr(7);
        } else if (!line.empty() && found.empty() && line.find('=') == std::string::npos) {
            found = line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return found;
}

} // namespace

std::expected<Document, StoreError> verify_document(std::string_view encoded_input) {
    const std::string encoded = trimmed(encoded_input);
    const auto bytes = from_base64url(encoded);
    if (!bytes) {
        return std::unexpected(invalid("The identity document is not base64url. Run liquid "
                                       "identity show --document and paste the complete output."));
    }
    const auto document = liquid::decode_identity_document(*bytes);
    if (!document) {
        return std::unexpected(invalid("The identity document is not a TAAP/1 identity document: " +
                                       document.error().message));
    }
    const auto verified = liquid::verify_document(*document);
    const auto canonical = liquid::encode_identity_document(*document);
    if (!verified || !*verified || !canonical || *canonical != *bytes) {
        return std::unexpected(invalid("The identity document's signature does not verify."));
    }
    // An initial document's identifier is its own key's fingerprint, so the key proves the
    // identifier. A successor's identifier is only proven by its chain, which this document
    // does not carry.
    if (document->previous_document_hash ||
        document->subject.fingerprint != document->subject.key_fingerprint) {
        return std::unexpected(
            invalid("This identity has been rotated, so its document alone does not prove which "
                    "identity it belongs to. Lake accepts an identity's first document."));
    }
    const auto hash = sha512_256_hex(*bytes);
    if (!hash) {
        return std::unexpected(StoreError{"store_error", "could not hash the identity document"});
    }
    return Document{document->subject.taap_id,
                    document->subject.key_fingerprint,
                    document->subject.algorithm,
                    encoded,
                    *hash};
}

nlohmann::json statement(const std::string& user_id,
                         const std::string& account_taap_id,
                         const Document& own,
                         const std::string& nonce,
                         const std::string& expires_at) {
    return {{"account", user_id},
            {"expires_at", expires_at},
            {"next", own.taap_id},
            {"next_document_hash", own.hash},
            {"nonce", nonce},
            {"previous", account_taap_id},
            {"purpose", "lake_use_own_identity"},
            {"version", link_version}};
}

std::string challenge(const nlohmann::json& value) {
    const auto encoded = as_bytes(nlohmann::json::to_cbor(value));
    const auto digest = liquid::hash(liquid::algorithm::sha256, encoded);
    return digest ? liquid::to_string(*digest) : std::string();
}

std::expected<nlohmann::json, StoreError>
verify_challenge_signature(std::string_view document_text,
                           std::string_view challenge_text,
                           std::string_view signed_output) {
    const auto bytes = from_base64url(trimmed(document_text));
    if (!bytes) {
        return std::unexpected(invalid("The identity document is not base64url."));
    }
    const auto document = liquid::decode_identity_document(*bytes);
    if (!document) {
        return std::unexpected(invalid("The identity document is invalid."));
    }
    std::string signer;
    const std::string signature_text = signature_hex(signed_output, signer);
    if (!signer.empty() && signer != document->subject.taap_id) {
        return std::unexpected(StoreError{
            "unauthorized",
            "That signature was made by " + signer + ", not by the identity you are linking (" +
                document->subject.taap_id + "). Run liquid sign with that identity."});
    }
    const auto signature_bytes = from_hex(signature_text);
    if (!signature_bytes) {
        return std::unexpected(invalid("Paste the output of liquid sign artifact <challenge>: "
                                       "its signature= line is hexadecimal."));
    }
    const auto signed_value = liquid::decode_signature(*signature_bytes);
    if (!signed_value) {
        return std::unexpected(
            invalid("The signature is not a TAAP/1 signature: " + signed_value.error().message));
    }
    // `liquid sign artifact <digest>` signs the digest text as a canonical CBOR text string.
    const auto body =
        as_bytes(nlohmann::json::to_cbor(nlohmann::json(std::string(challenge_text))));
    const auto verified = liquid::verify(document->subject, "artifact", body, *signed_value);
    if (!verified || !*verified) {
        return std::unexpected(StoreError{
            "unauthorized",
            "The signature does not verify with that identity's key for this challenge."});
    }
    return nlohmann::json{{"algorithm", signed_value->algorithm},
                          {"signing_key_identifier", signed_value->signing_key_identifier},
                          {"signature_purpose", signed_value->signature_purpose},
                          {"signed_at", signed_value->signed_at},
                          {"signature_hex", signature_text}};
}

std::string enrollment_challenge(std::span<const unsigned char> instance_kek,
                                 std::string_view pool_id,
                                 const Document& document,
                                 std::string_view nonce,
                                 std::int64_t expires_at) {
    // A key of its own, so the instance key never MACs request-shaped bytes directly.
    static constexpr std::string_view label = "lake identity enrollment challenge/1";
    std::array<unsigned char, EVP_MAX_MD_SIZE> key{};
    unsigned int key_size = 0;
    if (instance_kek.empty() || HMAC(EVP_sha256(),
                                     instance_kek.data(),
                                     static_cast<int>(instance_kek.size()),
                                     reinterpret_cast<const unsigned char*>(label.data()),
                                     label.size(),
                                     key.data(),
                                     &key_size) == nullptr) {
        return {};
    }
    const std::vector<std::uint8_t> statement =
        nlohmann::json::to_cbor(nlohmann::json{{"document_hash", document.hash},
                                               {"expires_at", expires_at},
                                               {"key_fingerprint", document.key_fingerprint},
                                               {"nonce", nonce},
                                               {"pool", pool_id},
                                               {"purpose", enrollment_purpose},
                                               {"taap_id", document.taap_id}});
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_size = 0;
    if (HMAC(EVP_sha256(),
             key.data(),
             static_cast<int>(key_size),
             statement.data(),
             statement.size(),
             mac.data(),
             &mac_size) == nullptr) {
        return {};
    }
    return "sha256:" + hex(std::span<const unsigned char>(mac.data(), mac_size));
}

nlohmann::json link_body(const IdentityLinkChallenge& value, const std::string& linked_at) {
    return {{"account", value.user_id},
            {"challenge", value.challenge},
            {"linked_at", linked_at},
            {"next", value.own_taap_id},
            {"next_document_hash", value.own_document_hash},
            {"previous", value.account_taap_id},
            {"statement", value.statement},
            {"version", link_version}};
}

std::expected<void, StoreError> verify_link(const nlohmann::json& link,
                                            std::string_view managed_public_key,
                                            std::string_view own_document) {
    if (!link.is_object() || !link.contains("body") || !link.contains("previous_signature") ||
        !link.contains("next_signature")) {
        return std::unexpected(invalid("the link document is incomplete"));
    }
    const nlohmann::json& body = link.at("body");
    const auto managed = audit::verify_managed_signature(
        managed_public_key, link_artifact, body, link.at("previous_signature").get<std::string>());
    if (!managed) {
        return std::unexpected(
            StoreError{"unauthorized", "the managed key's signature does not verify"});
    }
    if (challenge(body.at("statement")) != body.at("challenge").get<std::string>()) {
        return std::unexpected(
            StoreError{"unauthorized", "the challenge does not match the statement"});
    }
    const auto own = verify_challenge_signature(
        own_document,
        body.at("challenge").get<std::string>(),
        link.at("next_signature").at("signature_hex").get<std::string>());
    if (!own) {
        return std::unexpected(own.error());
    }
    return {};
}

} // namespace lake::control::own_identity
