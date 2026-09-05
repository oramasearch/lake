#include "lake/audit/audit.hpp"

#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "lake/control/memory_store.hpp"
#include "lake/control/store.hpp"

namespace lake::audit {
namespace {

constexpr std::string_view audit_signature_domain = "TAAP/1 audit_event/1";
constexpr std::string_view attestation_signature_domain = "TAAP/1 attestation";
constexpr std::string_view export_signature_domain = "TAAP/1 lake_export/1";
constexpr std::string_view checkpoint_signature_domain = "TAAP/1 lake_checkpoint/1";

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

nlohmann::json nullable(const std::string& value) {
    if (value.empty()) {
        return nullptr;
    }
    return value;
}

std::vector<std::uint8_t> cbor(const nlohmann::json& value) {
    return nlohmann::json::to_cbor(value);
}

nlohmann::json payload(const Event& event) {
    return {
        {"action", event.draft.action},
        {"customer_id", nullable(event.draft.customer_id)},
        {"details", event.draft.details},
        {"device", nullable(event.draft.device)},
        {"id", event.id},
        {"kind", event.draft.kind},
        {"method", nullable(event.draft.method)},
        {"object", event.draft.object},
        {"occurred_at", event.occurred_at},
        {"outcome", event.draft.outcome},
        {"previous_digest", nullable(event.previous_digest)},
        {"principal", event.draft.principal},
        {"request_id", event.draft.request_id},
        {"version", "audit_event/1"},
    };
}

nlohmann::json stored(const Event& event) {
    nlohmann::json value = payload(event);
    value["signature"] = event.signature;
    value["signer"] = event.signer;
    return value;
}

Result<std::string> base64_encode(const std::vector<std::uint8_t>& input) {
    if (input.empty()) {
        return std::string{};
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     input.data(),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(Error{"crypto_error", "could not encode base64"});
    }
    output.resize(static_cast<std::size_t>(size));
    return output;
}

Result<std::vector<std::uint8_t>> base64_decode(const std::string& input) {
    if (input.empty() || input.size() % 4 != 0) {
        return std::unexpected(Error{"invalid_input", "identity seed is not base64"});
    }
    std::vector<std::uint8_t> output((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(output.data(),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(Error{"invalid_input", "identity seed is not base64"});
    }
    std::size_t padding = 0;
    if (input.ends_with("=")) {
        ++padding;
    }
    if (input.ends_with("==")) {
        ++padding;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

Result<std::vector<std::uint8_t>> digest_bytes(const std::vector<std::uint8_t>& input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), output.data(), &size, EVP_sha512_256(), nullptr) !=
        1) {
        return std::unexpected(Error{"crypto_error", "could not calculate SHA-512/256"});
    }
    return std::vector<std::uint8_t>(output.begin(), output.begin() + size);
}

Result<std::string> digest_hex(const std::vector<std::uint8_t>& input) {
    auto bytes = digest_bytes(input);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    std::string output;
    output.reserve(bytes->size() * 2);
    for (const std::uint8_t byte : *bytes) {
        output += std::format("{:02x}", byte);
    }
    return output;
}

Result<Pkey> pkey_from_seed(const std::vector<std::uint8_t>& seed) {
    if (seed.size() != 32) {
        return std::unexpected(Error{"invalid_input", "an Ed25519 seed must be 32 bytes"});
    }
    Pkey key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    if (!key) {
        return std::unexpected(Error{"crypto_error", "could not create the Ed25519 private key"});
    }
    return key;
}

Result<std::vector<std::uint8_t>> public_key_of(EVP_PKEY* key) {
    std::vector<std::uint8_t> public_key(32);
    std::size_t size = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key, public_key.data(), &size) != 1 ||
        size != public_key.size()) {
        return std::unexpected(Error{"crypto_error", "could not read the Ed25519 public key"});
    }
    return public_key;
}

Result<std::string> sign(EVP_PKEY* key, const std::vector<std::uint8_t>& value) {
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(Error{"crypto_error", "could not allocate a signing context"});
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
    if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) != 1) {
        return std::unexpected(Error{"crypto_error", "could not initialize the Ed25519 signature"});
    }
    std::size_t size = 0;
    if (EVP_DigestSign(context.get(), nullptr, &size, value.data(), value.size()) != 1) {
        return std::unexpected(Error{"crypto_error", "could not determine signature size"});
    }
    std::vector<std::uint8_t> signature(size);
    if (EVP_DigestSign(context.get(), signature.data(), &size, value.data(), value.size()) != 1) {
        return std::unexpected(Error{"crypto_error", "could not sign audit event"});
    }
    signature.resize(size);
    return base64_encode(signature);
}

Result<void> verify_signature(const std::vector<std::uint8_t>& public_key,
                              const std::vector<std::uint8_t>& value,
                              const std::string& encoded_signature) {
    auto signature = base64_decode(encoded_signature);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    Pkey key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()));
    if (!key) {
        return std::unexpected(Error{"crypto_error", "could not create the Ed25519 public key"});
    }
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(Error{"crypto_error", "could not allocate a verification context"});
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
    if (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return std::unexpected(
            Error{"crypto_error", "could not initialize the Ed25519 verification"});
    }
    if (EVP_DigestVerify(
            context.get(), signature->data(), signature->size(), value.data(), value.size()) != 1) {
        return std::unexpected(Error{"invalid_signature", "audit event signature does not verify"});
    }
    return {};
}

std::string base32(const std::vector<std::uint8_t>& input) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t byte : input) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            output += alphabet[(buffer >> bits) & 0x1f];
        }
    }
    if (bits > 0) {
        output += alphabet[(buffer << (5 - bits)) & 0x1f];
    }
    return output;
}

Result<std::vector<std::uint8_t>> random_seed() {
    std::vector<std::uint8_t> seed(32);
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
        return std::unexpected(Error{"crypto_error", "could not generate the Ed25519 seed"});
    }
    return seed;
}

Result<std::string> wrap_seed(const std::vector<std::uint8_t>& seed,
                              const std::string& instance_kek) {
    if (instance_kek.empty()) {
        return std::unexpected(Error{
            "invalid_input", "LAKE_INSTANCE_KEK is required to persist a generated identity"});
    }
    const std::vector<std::uint8_t> kek(instance_kek.begin(), instance_kek.end());
    auto key = digest_bytes(kek);
    if (!key) {
        return std::unexpected(key.error());
    }
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 16> tag{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return std::unexpected(Error{"crypto_error", "could not generate the wrapping nonce"});
    }
    EVP_CIPHER_CTX* raw_context = EVP_CIPHER_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(Error{"crypto_error", "could not allocate the wrapping context"});
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw_context,
                                                                            EVP_CIPHER_CTX_free);
    if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key->data(), nonce.data()) !=
        1) {
        return std::unexpected(Error{"crypto_error", "could not initialize identity wrapping"});
    }
    std::vector<std::uint8_t> ciphertext(seed.size() + EVP_CIPHER_block_size(EVP_aes_256_gcm()));
    int encrypted = 0;
    if (EVP_EncryptUpdate(context.get(),
                          ciphertext.data(),
                          &encrypted,
                          seed.data(),
                          static_cast<int>(seed.size())) != 1) {
        return std::unexpected(Error{"crypto_error", "could not wrap the identity seed"});
    }
    int final_size = 0;
    if (EVP_EncryptFinal_ex(context.get(), ciphertext.data() + encrypted, &final_size) != 1) {
        return std::unexpected(Error{"crypto_error", "could not finish identity wrapping"});
    }
    ciphertext.resize(static_cast<std::size_t>(encrypted + final_size));
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) {
        return std::unexpected(Error{"crypto_error", "could not read the identity wrapping tag"});
    }
    std::vector<std::uint8_t> envelope;
    envelope.insert(envelope.end(), nonce.begin(), nonce.end());
    envelope.insert(envelope.end(), tag.begin(), tag.end());
    envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
    return base64_encode(envelope);
}

Result<std::vector<std::uint8_t>> unwrap_seed(const std::string& wrapped_seed,
                                              const std::string& instance_kek) {
    if (instance_kek.empty()) {
        return std::unexpected(
            Error{"invalid_input", "LAKE_INSTANCE_KEK is required to load the generated identity"});
    }
    auto envelope = base64_decode(wrapped_seed);
    if (!envelope || envelope->size() < 12 + 16 + 1) {
        return std::unexpected(
            Error{"invalid_input", "stored identity seed is not a valid wrapped envelope"});
    }
    const std::vector<std::uint8_t> kek(instance_kek.begin(), instance_kek.end());
    auto key = digest_bytes(kek);
    if (!key) {
        return std::unexpected(key.error());
    }
    EVP_CIPHER_CTX* raw_context = EVP_CIPHER_CTX_new();
    if (raw_context == nullptr) {
        return std::unexpected(Error{"crypto_error", "could not allocate the unwrapping context"});
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw_context,
                                                                            EVP_CIPHER_CTX_free);
    const std::uint8_t* nonce = envelope->data();
    const std::uint8_t* tag = envelope->data() + 12;
    const std::uint8_t* ciphertext = envelope->data() + 28;
    const int ciphertext_size = static_cast<int>(envelope->size() - 28);
    if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key->data(), nonce) != 1) {
        return std::unexpected(Error{"crypto_error", "could not initialize identity unwrapping"});
    }
    std::vector<std::uint8_t> seed(static_cast<std::size_t>(ciphertext_size));
    int decrypted = 0;
    if (EVP_DecryptUpdate(context.get(), seed.data(), &decrypted, ciphertext, ciphertext_size) !=
        1) {
        return std::unexpected(Error{"crypto_error", "could not unwrap the identity seed"});
    }
    if (EVP_CIPHER_CTX_ctrl(
            context.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t*>(tag)) != 1) {
        return std::unexpected(Error{"crypto_error", "could not set the identity wrapping tag"});
    }
    int final_size = 0;
    if (EVP_DecryptFinal_ex(context.get(), seed.data() + decrypted, &final_size) != 1) {
        return std::unexpected(
            Error{"invalid_input", "could not authenticate the stored identity seed"});
    }
    seed.resize(static_cast<std::size_t>(decrypted + final_size));
    return seed;
}

std::string timestamp() {
    const auto now =
        std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", now);
}

Result<std::vector<std::uint8_t>> seed_for(const Identity& identity,
                                           const IdentityOptions& options) {
    if (identity.wrapped_seed.empty()) {
        if (options.configured_seed.empty()) {
            return std::unexpected(
                Error{"invalid_input", "LAKE_IDENTITY_SEED is required for this identity"});
        }
        auto seed = base64_decode(options.configured_seed);
        if (!seed) {
            return std::unexpected(seed.error());
        }
        if (seed->size() != 32) {
            return std::unexpected(
                Error{"invalid_input", "LAKE_IDENTITY_SEED must decode to 32 bytes"});
        }
        return seed;
    }
    return unwrap_seed(identity.wrapped_seed, options.instance_kek);
}

Result<Identity> identity_from_seed(const std::vector<std::uint8_t>& seed,
                                    const IdentityOptions& options) {
    auto key = pkey_from_seed(seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto public_key = public_key_of(key->get());
    if (!public_key) {
        return std::unexpected(public_key.error());
    }
    auto encoded_public_key = base64_encode(*public_key);
    if (!encoded_public_key) {
        return std::unexpected(encoded_public_key.error());
    }
    auto fingerprint = digest_bytes(*public_key);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    std::string wrapped_seed;
    if (options.configured_seed.empty()) {
        auto generated_seed = wrap_seed(seed, options.instance_kek);
        if (!generated_seed) {
            return std::unexpected(generated_seed.error());
        }
        wrapped_seed = *generated_seed;
    }
    return Identity{
        .id = control::new_uuid_v4(),
        .taap_id = "taap:1:ed25519:" + base32(*fingerprint),
        .public_key = *encoded_public_key,
        .wrapped_seed = std::move(wrapped_seed),
        .created_at = timestamp(),
        .rotation_document = {},
        .current = true,
    };
}

Result<std::vector<std::uint8_t>> signed_bytes(const Event& event) {
    std::vector<std::uint8_t> output(audit_signature_domain.begin(), audit_signature_domain.end());
    output.push_back(0);
    const auto encoded = cbor(payload(event));
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

std::vector<std::uint8_t> attestation_signed_bytes(const nlohmann::json& body) {
    std::vector<std::uint8_t> output(attestation_signature_domain.begin(),
                                     attestation_signature_domain.end());
    output.push_back(0);
    const auto encoded = cbor(body);
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

std::vector<std::uint8_t> export_manifest_signed_bytes(const nlohmann::json& manifest) {
    std::vector<std::uint8_t> output(export_signature_domain.begin(),
                                     export_signature_domain.end());
    output.push_back(0);
    const auto encoded = cbor(manifest);
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

std::vector<std::uint8_t> checkpoint_signed_bytes(const nlohmann::json& checkpoint) {
    std::vector<std::uint8_t> output(checkpoint_signature_domain.begin(),
                                     checkpoint_signature_domain.end());
    output.push_back(0);
    const auto encoded = cbor(checkpoint);
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

Result<Event> signed_event(const EventDraft& draft,
                           const Identity& identity,
                           const IdentityOptions& options,
                           const std::string& previous_digest) {
    auto seed = seed_for(identity, options);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    auto key = pkey_from_seed(*seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    Event event{
        .id = control::new_uuid_v4(),
        .occurred_at = timestamp(),
        .draft = draft,
        .previous_digest = previous_digest,
        .signer = identity.taap_id,
    };
    auto bytes = signed_bytes(event);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto signature = sign(key->get(), *bytes);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    event.signature = *signature;
    auto digest = digest_hex(cbor(stored(event)));
    if (!digest) {
        return std::unexpected(digest.error());
    }
    event.digest = *digest;
    return event;
}

Error from_store_error(const control::StoreError& error) {
    return Error{error.code, error.message};
}

IdentityOptions options_for_store(control::Store& store, IdentityOptions options) {
    if (options.instance_kek.empty() && store.kind() == "memory") {
        options.instance_kek = "lake-memory-store-instance-kek";
    }
    return options;
}

} // namespace

nlohmann::json to_json(const Event& event) {
    nlohmann::json value = stored(event);
    value["digest"] = event.digest;
    return value;
}

Event event_from_json(const nlohmann::json& value) {
    const auto text = [&value](const char* key) -> std::string {
        if (!value.contains(key) || value.at(key).is_null()) {
            return {};
        }
        return value.at(key).get<std::string>();
    };
    Event event;
    event.id = text("id");
    event.occurred_at = text("occurred_at");
    event.draft.customer_id = text("customer_id");
    event.draft.kind = text("kind");
    event.draft.principal = text("principal");
    event.draft.object = text("object");
    event.draft.action = text("action");
    event.draft.outcome = text("outcome");
    event.draft.request_id = text("request_id");
    event.draft.method = text("method");
    event.draft.device = text("device");
    event.draft.details = value.value("details", nlohmann::json::object());
    event.previous_digest = text("previous_digest");
    event.signer = text("signer");
    event.signature = text("signature");
    event.digest = text("digest");
    return event;
}

nlohmann::json identity_document(const Identity& identity) {
    return {
        {"created_at", identity.created_at},
        {"public_key", identity.public_key},
        {"taap_id", identity.taap_id},
        {"version", "taap_identity/1"},
    };
}

std::string json_lines(const std::vector<Event>& events) {
    std::string output;
    for (const Event& event : events) {
        output += to_json(event).dump();
        output += '\n';
    }
    return output;
}

Result<Identity> identity(control::Store& store, const IdentityOptions& options) {
    const IdentityOptions effective_options = options_for_store(store, options);
    auto current = store.current_lake_identity();
    if (current) {
        if (current->wrapped_seed.empty()) {
            auto seed = base64_decode(effective_options.configured_seed);
            if (!seed) {
                return std::unexpected(seed.error());
            }
            auto configured = identity_from_seed(*seed, effective_options);
            if (!configured) {
                return std::unexpected(configured.error());
            }
            if (configured->taap_id != current->taap_id) {
                return std::unexpected(Error{
                    "conflict", "LAKE_IDENTITY_SEED does not match the stored lake identity"});
            }
        }
        return *current;
    }
    if (current.error().code != "not_found") {
        return std::unexpected(Error{current.error().code, current.error().message});
    }
    Result<std::vector<std::uint8_t>> seed = effective_options.configured_seed.empty()
                                                 ? random_seed()
                                                 : base64_decode(effective_options.configured_seed);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    auto created = identity_from_seed(*seed, effective_options);
    if (!created) {
        return std::unexpected(created.error());
    }
    auto saved = store.create_lake_identity(*created);
    if (saved) {
        return *created;
    }
    if (saved.error().code != "conflict") {
        return std::unexpected(Error{saved.error().code, saved.error().message});
    }
    current = store.current_lake_identity();
    if (!current) {
        return std::unexpected(Error{current.error().code, current.error().message});
    }
    return *current;
}

Result<nlohmann::json> sign_passkey_binding(control::Store& store,
                                            const std::string& subject_taap_id,
                                            const std::string& credential_id,
                                            const std::string& pool_id,
                                            const IdentityOptions& options) {
    if (subject_taap_id.empty() || credential_id.empty() || pool_id.empty()) {
        return std::unexpected(Error{"invalid_input", "passkey binding fields are required"});
    }
    const IdentityOptions effective_options = options_for_store(store, options);
    const auto current = identity(store, effective_options);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto seed = seed_for(*current, effective_options);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const auto key = pkey_from_seed(*seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    const std::string issued_at = timestamp();
    nlohmann::json attestation = {
        {"artifact_type", "attestation"},
        {"protocol_version", "TAAP/1"},
        {"attestation_identifier", control::new_uuid_v4()},
        {"authority_identifier", current->taap_id},
        {"authority_display_name", "lake"},
        {"subject_identifier", subject_taap_id},
        {"attestation_profile", "passkey_binding/1"},
        {"asserted_attributes", {{"credential_id", credential_id}, {"pool_id", pool_id}}},
        {"evidence_references", nlohmann::json::array()},
        {"issued_at", issued_at},
        {"valid_from", issued_at},
        {"expires_at", nullptr},
        {"status_policy",
         {{"status_check_recommended", true},
          {"status_authority_identifier", current->taap_id},
          {"maximum_status_age_seconds", 300}}},
    };
    const auto signature = sign(key->get(), attestation_signed_bytes(attestation));
    if (!signature) {
        return std::unexpected(signature.error());
    }
    attestation["signatures"] =
        nlohmann::json::array({{{"signature_algorithm", "ed25519"},
                                {"signing_key_identifier", current->taap_id},
                                {"signing_key_label", "lake-instance"},
                                {"signature_purpose", "attestation"},
                                {"signed_at", issued_at},
                                {"signature_bytes_base64", *signature}}});
    return attestation;
}

Result<void> verify_passkey_binding(control::Store& store,
                                    const nlohmann::json& attestation,
                                    const std::string& subject_taap_id,
                                    const std::string& credential_id,
                                    const std::string& pool_id) {
    const auto text = [&](const nlohmann::json& object, std::string_view name) -> std::string {
        const std::string key(name);
        if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
            return {};
        }
        return object.at(key).get<std::string>();
    };
    if (text(attestation, "artifact_type") != "attestation" ||
        text(attestation, "protocol_version") != "TAAP/1" ||
        text(attestation, "attestation_profile") != "passkey_binding/1" ||
        text(attestation, "subject_identifier") != subject_taap_id ||
        !attestation.contains("asserted_attributes") ||
        text(attestation.at("asserted_attributes"), "credential_id") != credential_id ||
        text(attestation.at("asserted_attributes"), "pool_id") != pool_id ||
        !attestation.contains("signatures") || !attestation.at("signatures").is_array() ||
        attestation.at("signatures").size() != 1) {
        return std::unexpected(Error{"invalid_input", "passkey binding fields do not match"});
    }
    const nlohmann::json& signature = attestation.at("signatures").front();
    const std::string authority = text(attestation, "authority_identifier");
    if (authority.empty() || text(signature, "signature_algorithm") != "ed25519" ||
        text(signature, "signing_key_identifier") != authority ||
        text(signature, "signature_purpose") != "attestation") {
        return std::unexpected(Error{"invalid_input", "passkey binding signature is malformed"});
    }
    const auto signing_identity = store.lake_identity_by_taap_id(authority);
    if (!signing_identity) {
        return std::unexpected(
            Error{signing_identity.error().code, signing_identity.error().message});
    }
    const auto public_key = base64_decode(signing_identity->public_key);
    if (!public_key) {
        return std::unexpected(public_key.error());
    }
    nlohmann::json body = attestation;
    body.erase("signatures");
    return verify_signature(
        *public_key, attestation_signed_bytes(body), text(signature, "signature_bytes_base64"));
}

Result<nlohmann::json> sign_offline_grant(control::Store& store,
                                          const std::string& grant_id,
                                          const std::string& subject_taap_id,
                                          const std::string& device_id,
                                          const std::vector<std::string>& scopes,
                                          std::int64_t expires_unix,
                                          const IdentityOptions& options) {
    if (grant_id.empty() || subject_taap_id.empty() || device_id.empty() || scopes.empty() ||
        expires_unix <= 0) {
        return std::unexpected(Error{"invalid_input", "offline grant fields are required"});
    }
    const IdentityOptions effective_options = options_for_store(store, options);
    const auto current = identity(store, effective_options);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto seed = seed_for(*current, effective_options);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const auto key = pkey_from_seed(*seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    const std::string issued_at = timestamp();
    const std::string expires_at =
        std::format("{:%FT%TZ}", std::chrono::sys_seconds{std::chrono::seconds{expires_unix}});
    nlohmann::json attestation = {
        {"artifact_type", "attestation"},
        {"protocol_version", "TAAP/1"},
        {"attestation_identifier", grant_id},
        {"authority_identifier", current->taap_id},
        {"authority_display_name", "lake"},
        {"subject_identifier", subject_taap_id},
        {"attestation_profile", "offline_grant/1"},
        {"asserted_attributes",
         {{"device_id", device_id}, {"expires_unix", expires_unix}, {"scopes", scopes}}},
        {"evidence_references", nlohmann::json::array()},
        {"issued_at", issued_at},
        {"valid_from", issued_at},
        {"expires_at", expires_at},
        {"status_policy",
         {{"status_check_recommended", false},
          {"status_authority_identifier", current->taap_id},
          {"maximum_status_age_seconds", 0}}},
    };
    const auto signature = sign(key->get(), attestation_signed_bytes(attestation));
    if (!signature) {
        return std::unexpected(signature.error());
    }
    attestation["signatures"] =
        nlohmann::json::array({{{"signature_algorithm", "ed25519"},
                                {"signing_key_identifier", current->taap_id},
                                {"signing_key_label", "lake-instance"},
                                {"signature_purpose", "attestation"},
                                {"signed_at", issued_at},
                                {"signature_bytes_base64", *signature}}});
    return attestation;
}

Result<void> verify_offline_grant(control::Store& store,
                                  const nlohmann::json& attestation,
                                  const std::string& scope,
                                  std::int64_t now_unix) {
    const auto text = [&](const nlohmann::json& object, std::string_view name) -> std::string {
        const std::string key(name);
        if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
            return {};
        }
        return object.at(key).get<std::string>();
    };
    if (text(attestation, "artifact_type") != "attestation" ||
        text(attestation, "protocol_version") != "TAAP/1" ||
        text(attestation, "attestation_profile") != "offline_grant/1" ||
        !attestation.contains("asserted_attributes") ||
        !attestation.at("asserted_attributes").is_object() || !attestation.contains("signatures") ||
        !attestation.at("signatures").is_array() || attestation.at("signatures").size() != 1) {
        return std::unexpected(Error{"invalid_input", "offline grant is malformed"});
    }
    const nlohmann::json& attributes = attestation.at("asserted_attributes");
    if (!attributes.contains("expires_unix") ||
        !attributes.at("expires_unix").is_number_integer() || !attributes.contains("scopes") ||
        !attributes.at("scopes").is_array()) {
        return std::unexpected(Error{"invalid_input", "offline grant attributes are malformed"});
    }
    if (now_unix >= attributes.at("expires_unix").get<std::int64_t>()) {
        return std::unexpected(Error{"unauthorized", "offline grant has expired"});
    }
    const auto& scopes = attributes.at("scopes");
    if (std::ranges::none_of(scopes, [&](const nlohmann::json& item) {
            return item.is_string() && item.get<std::string>() == scope;
        })) {
        return std::unexpected(Error{"unauthorized", "offline grant does not include the scope"});
    }
    const nlohmann::json& signature = attestation.at("signatures").front();
    const std::string authority = text(attestation, "authority_identifier");
    if (authority.empty() || text(signature, "signature_algorithm") != "ed25519" ||
        text(signature, "signing_key_identifier") != authority ||
        text(signature, "signature_purpose") != "attestation") {
        return std::unexpected(Error{"invalid_input", "offline grant signature is malformed"});
    }
    const auto signing_identity = store.lake_identity_by_taap_id(authority);
    if (!signing_identity) {
        return std::unexpected(
            Error{signing_identity.error().code, signing_identity.error().message});
    }
    const auto public_key = base64_decode(signing_identity->public_key);
    if (!public_key) {
        return std::unexpected(public_key.error());
    }
    nlohmann::json body = attestation;
    body.erase("signatures");
    return verify_signature(
        *public_key, attestation_signed_bytes(body), text(signature, "signature_bytes_base64"));
}

Result<Event>
record(control::Store& store, const EventDraft& draft, const IdentityOptions& options) {
    const IdentityOptions effective_options = options_for_store(store, options);
    auto current = identity(store, effective_options);
    if (!current) {
        return std::unexpected(current.error());
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto existing = store.latest_audit_event();
        if (!existing) {
            if (existing.error().code != "not_found") {
                return std::unexpected(from_store_error(existing.error()));
            }
        }
        const std::string previous_digest = existing ? existing->digest : "";
        auto event = signed_event(draft, *current, effective_options, previous_digest);
        if (!event) {
            return std::unexpected(event.error());
        }
        auto appended = store.append_audit_event(*event);
        if (appended) {
            return *event;
        }
        if (appended.error().code != "conflict") {
            return std::unexpected(from_store_error(appended.error()));
        }
    }
    return std::unexpected(
        Error{"conflict", "could not append audit event after concurrent updates"});
}

Result<void> verify(control::Store& store, const std::vector<Event>& events) {
    std::string previous_digest;
    for (const Event& event : events) {
        if (event.previous_digest != previous_digest) {
            return std::unexpected(
                Error{"invalid_chain", "audit event predecessor digest does not match"});
        }
        auto identity_row = store.lake_identity_by_taap_id(event.signer);
        if (!identity_row) {
            return std::unexpected(Error{identity_row.error().code, identity_row.error().message});
        }
        auto public_key = base64_decode(identity_row->public_key);
        if (!public_key) {
            return std::unexpected(public_key.error());
        }
        auto bytes = signed_bytes(event);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto signature = verify_signature(*public_key, *bytes, event.signature);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        auto digest = digest_hex(cbor(stored(event)));
        if (!digest) {
            return std::unexpected(digest.error());
        }
        if (*digest != event.digest) {
            return std::unexpected(
                Error{"invalid_digest", "audit event digest does not match its contents"});
        }
        previous_digest = event.digest;
    }
    return {};
}

Result<Identity> rotate_identity(control::Store& store, const IdentityOptions& options) {
    const IdentityOptions effective_options = options_for_store(store, options);
    auto previous = identity(store, effective_options);
    if (!previous) {
        return std::unexpected(previous.error());
    }
    auto previous_seed = seed_for(*previous, effective_options);
    if (!previous_seed) {
        return std::unexpected(previous_seed.error());
    }
    auto previous_key = pkey_from_seed(*previous_seed);
    if (!previous_key) {
        return std::unexpected(previous_key.error());
    }
    auto next_seed = random_seed();
    if (!next_seed) {
        return std::unexpected(next_seed.error());
    }
    IdentityOptions replacement_options = effective_options;
    replacement_options.configured_seed.clear();
    auto replacement = identity_from_seed(*next_seed, replacement_options);
    if (!replacement) {
        return std::unexpected(replacement.error());
    }
    auto next_key = pkey_from_seed(*next_seed);
    if (!next_key) {
        return std::unexpected(next_key.error());
    }
    nlohmann::json document = {
        {"created_at", replacement->created_at},
        {"next", replacement->taap_id},
        {"previous", previous->taap_id},
        {"version", "taap_rotation/1"},
    };
    const auto document_bytes = cbor(document);
    auto old_signature = sign(previous_key->get(), document_bytes);
    if (!old_signature) {
        return std::unexpected(old_signature.error());
    }
    auto new_signature = sign(next_key->get(), document_bytes);
    if (!new_signature) {
        return std::unexpected(new_signature.error());
    }
    document["new_signature"] = *new_signature;
    document["previous_signature"] = *old_signature;
    replacement->rotation_document = document.dump();
    auto rotated = store.rotate_lake_identity(*previous, *replacement);
    if (!rotated) {
        return std::unexpected(Error{rotated.error().code, rotated.error().message});
    }
    return *replacement;
}

Result<nlohmann::json> sign_transparency_checkpoint(control::Store& store,
                                                    const nlohmann::json& checkpoint,
                                                    const IdentityOptions& options) {
    const IdentityOptions effective_options = options_for_store(store, options);
    const auto current = identity(store, effective_options);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto seed = seed_for(*current, effective_options);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const auto key = pkey_from_seed(*seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    const auto signature = sign(key->get(), checkpoint_signed_bytes(checkpoint));
    if (!signature) {
        return std::unexpected(signature.error());
    }
    return nlohmann::json{
        {"algorithm", "ed25519"}, {"signer", current->taap_id}, {"signature", *signature}};
}

Result<void> verify_transparency_checkpoint(control::Store& store,
                                            const nlohmann::json& checkpoint,
                                            const nlohmann::json& signature) {
    if (!signature.is_object() || signature.value("algorithm", "") != "ed25519" ||
        !signature.contains("signer") || !signature.at("signer").is_string() ||
        !signature.contains("signature") || !signature.at("signature").is_string()) {
        return std::unexpected(Error{"invalid_input", "checkpoint signature is malformed"});
    }
    const auto identity = store.lake_identity_by_taap_id(signature.at("signer").get<std::string>());
    if (!identity) {
        return std::unexpected(Error{identity.error().code, identity.error().message});
    }
    const auto public_key = base64_decode(identity->public_key);
    if (!public_key || public_key->size() != 32) {
        return std::unexpected(Error{"invalid_input", "checkpoint signer key is malformed"});
    }
    return verify_signature(*public_key,
                            checkpoint_signed_bytes(checkpoint),
                            signature.at("signature").get<std::string>());
}

Result<nlohmann::json> sign_export_manifest(control::Store& store,
                                            const nlohmann::json& manifest,
                                            const IdentityOptions& options) {
    const IdentityOptions effective_options = options_for_store(store, options);
    const auto current = identity(store, effective_options);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto seed = seed_for(*current, effective_options);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    const auto key = pkey_from_seed(*seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    const auto signed_bytes = export_manifest_signed_bytes(manifest);
    const auto signature = sign(key->get(), signed_bytes);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    return nlohmann::json{{"identity", identity_document(*current)}, {"signature", *signature}};
}

Result<void> verify_export_manifest(const nlohmann::json& manifest,
                                    const nlohmann::json& signature) {
    if (!signature.is_object() || !signature.contains("identity") ||
        !signature.contains("signature") || !signature.at("identity").is_object() ||
        !signature.at("signature").is_string()) {
        return std::unexpected(Error{"invalid_input", "export manifest signature is malformed"});
    }
    const auto& identity = signature.at("identity");
    if (!identity.contains("version") || identity.value("version", "") != "taap_identity/1" ||
        !identity.contains("taap_id") || !identity.at("taap_id").is_string() ||
        !identity.contains("public_key") || !identity.at("public_key").is_string()) {
        return std::unexpected(Error{"invalid_input", "export signer identity is malformed"});
    }
    const auto public_key = base64_decode(identity.at("public_key").get<std::string>());
    if (!public_key || public_key->size() != 32) {
        return std::unexpected(Error{"invalid_input", "export signer public key is malformed"});
    }
    return verify_signature(*public_key,
                            export_manifest_signed_bytes(manifest),
                            signature.at("signature").get<std::string>());
}

Result<void> validate_retention_days(int days) {
    if (days < retention_floor_days) {
        return std::unexpected(Error{"invalid_input", "audit retention cannot be below 400 days"});
    }
    return {};
}

} // namespace lake::audit
