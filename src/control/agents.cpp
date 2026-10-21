#include "lake/control/agents.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <liquid/attestation.hpp>
#include <liquid/hash.hpp>
#include <liquid/identity.hpp>

#include "objects/records.hpp"

namespace lake::control::agents {
namespace {

using Bytes = std::vector<std::byte>;
using OrderedJson = nlohmann::ordered_json;

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

Error audit_error(const audit::Error& error) {
    return {error.code, error.message};
}

Error liquid_error(const liquid::error& error) {
    const std::string code =
        error.code == liquid::errc::unauthorized ? "unauthorized" : "invalid_input";
    return {code, error.message};
}

Bytes byte_values(std::span<const std::uint8_t> input) {
    Bytes output;
    output.reserve(input.size());
    for (const std::uint8_t value : input) {
        output.push_back(std::byte{value});
    }
    return output;
}

Result<std::string> base64(std::span<const std::byte> input, bool url) {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(Error{"invalid_input", "key material is too large"});
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(Error{"crypto_error", "could not encode key material"});
    }
    output.resize(static_cast<std::size_t>(size));
    if (url) {
        std::ranges::replace(output, '+', '-');
        std::ranges::replace(output, '/', '_');
        while (output.ends_with('=')) {
            output.pop_back();
        }
    }
    return output;
}

Result<Bytes> decode_base64(std::string input, bool url) {
    if (input.empty()) {
        return std::unexpected(Error{"invalid_input", "encoded key material is empty"});
    }
    if (url) {
        std::ranges::replace(input, '-', '+');
        std::ranges::replace(input, '_', '/');
        const std::size_t remainder = input.size() % 4;
        if (remainder == 1) {
            return std::unexpected(Error{"invalid_input", "key material is not valid base64url"});
        }
        if (remainder != 0) {
            input.append(4 - remainder, '=');
        }
    }
    if (input.size() % 4 != 0) {
        return std::unexpected(Error{"invalid_input", "key material is not valid base64"});
    }
    std::vector<std::uint8_t> decoded((input.size() / 4) * 3);
    const int size = EVP_DecodeBlock(decoded.data(),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    if (size < 0) {
        return std::unexpected(Error{"invalid_input", "key material is not valid base64"});
    }
    std::size_t padding = 0;
    if (input.ends_with('=')) {
        ++padding;
    }
    if (input.ends_with("==")) {
        ++padding;
    }
    decoded.resize(static_cast<std::size_t>(size) - padding);
    return byte_values(decoded);
}

Result<Bytes> canonical_cbor(const OrderedJson& value) {
    try {
        const std::vector<std::uint8_t> encoded = OrderedJson::to_cbor(value);
        return byte_values(encoded);
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(Error{"crypto_error", error.what()});
    }
}

Result<Bytes> canonical_public_key(std::span<const std::byte> public_key,
                                   std::string_view issued_at) {
    OrderedJson key = OrderedJson::object();
    key["key_label"] = "agent-session";
    key["key_usage"] = OrderedJson::array({"signing"});
    key["created_at"] = issued_at;
    key["key_algorithm"] = "ed25519";
    auto encoded_key = base64(public_key, false);
    if (!encoded_key) {
        return std::unexpected(encoded_key.error());
    }
    key["public_key_bytes_base64"] = *encoded_key;
    return canonical_cbor(key);
}

Result<Bytes> digest(std::span<const std::byte> input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), output.data(), &size, EVP_sha512_256(), nullptr) !=
            1 ||
        size != 32) {
        return std::unexpected(Error{"crypto_error", "could not calculate key fingerprint"});
    }
    return byte_values(std::span<const std::uint8_t>(output.data(), size));
}

std::string base32(std::span<const std::byte> input) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string output;
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const std::byte value : input) {
        buffer = (buffer << 8U) | std::to_integer<std::uint8_t>(value);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            output.push_back(alphabet[(buffer >> bits) & 0x1fU]);
        }
    }
    if (bits != 0) {
        output.push_back(alphabet[(buffer << (5U - bits)) & 0x1fU]);
    }
    return output;
}

Result<liquid::actor> session_actor(std::span<const std::byte> public_key,
                                    std::string_view issued_at) {
    auto key = canonical_public_key(public_key, issued_at);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto fingerprint = digest(*key);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    const std::string encoded = base32(*fingerprint);
    return liquid::actor{"taap:1:ed25519:" + encoded,
                         "ed25519",
                         encoded,
                         Bytes(public_key.begin(), public_key.end()),
                         encoded};
}

Result<liquid::actor> lake_actor(const audit::Identity& identity) {
    constexpr std::string_view prefix = "taap:1:ed25519:";
    if (!identity.taap_id.starts_with(prefix)) {
        return std::unexpected(Error{"crypto_error", "Lake identity is not an Ed25519 identity"});
    }
    auto public_key = decode_base64(identity.public_key, false);
    if (!public_key || public_key->size() != 32) {
        return std::unexpected(public_key ? Error{"crypto_error", "Lake public key is invalid"}
                                          : public_key.error());
    }
    const std::string fingerprint = identity.taap_id.substr(prefix.size());
    return liquid::actor{
        identity.taap_id, "ed25519", fingerprint, std::move(*public_key), fingerprint};
}

Result<Pkey> generate_key(std::array<std::uint8_t, 32>& seed) {
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
        return std::unexpected(Error{"crypto_error", "could not generate an agent session key"});
    }
    Pkey key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    if (!key) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return std::unexpected(Error{"crypto_error", "could not create an agent session key"});
    }
    return key;
}

Result<Bytes> public_key(EVP_PKEY* key) {
    std::array<std::uint8_t, 32> output{};
    std::size_t size = output.size();
    if (EVP_PKEY_get_raw_public_key(key, output.data(), &size) != 1 || size != output.size()) {
        return std::unexpected(
            Error{"crypto_error", "could not read the agent session public key"});
    }
    return byte_values(output);
}

Result<std::string> encode_delegation(Store& store,
                                      const liquid::actor& session,
                                      const std::string& dispatcher_taap_id,
                                      const std::vector<std::string>& scopes,
                                      std::chrono::sys_seconds issued,
                                      const audit::IdentityOptions& identity_options) {
    const auto lake_identity = audit::identity(store, identity_options);
    if (!lake_identity) {
        return std::unexpected(audit_error(lake_identity.error()));
    }
    auto issuer = lake_actor(*lake_identity);
    if (!issuer) {
        return std::unexpected(issuer.error());
    }
    auto subject_key = canonical_public_key(session.public_key, std::format("{:%FT%TZ}", issued));
    if (!subject_key) {
        return std::unexpected(subject_key.error());
    }
    auto subject = liquid::hash(liquid::algorithm::blake3, *subject_key);
    if (!subject) {
        return std::unexpected(liquid_error(subject.error()));
    }
    liquid::attestation statement{
        .subject = *subject,
        .profile = "delegation/1",
        .at = issued,
        .issuer = std::move(*issuer),
        .subject_kind = liquid::attestation_subject_kind::actor,
        .attributes = {{"scopes", scopes},
                       {"may_publish", false},
                       {"may_lease", false},
                       {"depth_remaining", std::uint64_t{0}},
                       {"dispatcher", dispatcher_taap_id}},
    };
    auto body = liquid::objects::encode_attestation_body(statement);
    if (!body) {
        return std::unexpected(liquid_error(body.error()));
    }
    auto raw_signature =
        audit::sign_taap(store, lake_identity->taap_id, "attestation", *body, identity_options);
    if (!raw_signature) {
        return std::unexpected(audit_error(raw_signature.error()));
    }
    liquid::signature signature{
        .algorithm = "ed25519",
        .signing_key_identifier = lake_identity->taap_id,
        .signing_key_label = "lake-instance",
        .signature_purpose = "attestation",
        .signed_at = std::format("{:%FT%TZ}", issued),
        .bytes = std::move(*raw_signature),
    };
    auto encoded_signature = liquid::encode_attestation_signature(signature);
    if (!encoded_signature) {
        return std::unexpected(liquid_error(encoded_signature.error()));
    }
    statement.signature = std::move(*encoded_signature);
    // Lake's instance key is not a liquid identity chain, so liquid::verify_attestation would
    // report an unknown signer (liquid#383). Check the profile schema and the signature against
    // the instance key Lake holds instead.
    if (auto valid = liquid::attestation_profile_registry::global().validate(statement); !valid) {
        return std::unexpected(
            Error{"crypto_error", "could not verify agent delegation: " + valid.error().message});
    }
    auto verified = liquid::verify_key_signature(statement.issuer, "attestation", *body, signature);
    if (!verified || !*verified) {
        return std::unexpected(
            Error{"crypto_error",
                  verified ? std::string{"could not verify agent delegation: unsigned"}
                           : "could not verify agent delegation: " + verified.error().message});
    }
    auto encoded = liquid::encode_attestation(statement);
    if (!encoded) {
        return std::unexpected(liquid_error(encoded.error()));
    }
    return base64(*encoded, true);
}

bool valid_taap_id(std::string_view value) {
    return value.starts_with("taap:1:") && value.size() > std::string_view("taap:1:").size();
}

Result<void> validate(const MintRequest& request) {
    if (request.customer_id.empty() || !valid_taap_id(request.durable_agent_taap_id) ||
        !valid_taap_id(request.dispatcher_taap_id) || request.scopes.empty()) {
        return std::unexpected(Error{"invalid_input", "agent session fields are required"});
    }
    if (!request.never_expires &&
        (request.lifetime_seconds <= 0 || request.lifetime_seconds > 900)) {
        return std::unexpected(
            Error{"invalid_input", "agent session lifetime must be at most fifteen minutes"});
    }
    if (std::ranges::any_of(request.scopes,
                            [](const std::string& scope) { return scope.empty(); })) {
        return std::unexpected(Error{"invalid_input", "agent session scopes must not be empty"});
    }
    std::vector<std::string> unique = request.scopes;
    std::ranges::sort(unique);
    if (std::ranges::adjacent_find(unique) != unique.end()) {
        return std::unexpected(Error{"invalid_input", "agent session scopes must be unique"});
    }
    return {};
}

} // namespace

Service::Service(Clock clock)
    : clock_(clock ? std::move(clock) : [] {
          return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
      }) {}

Result<MintedSession> Service::mint(Store& store,
                                    const MintRequest& request,
                                    const audit::IdentityOptions& identity_options) const {
    auto valid = validate(request);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    const auto issued = clock_();
    const auto expires = issued + std::chrono::seconds(request.lifetime_seconds);
    const std::string issued_at = std::format("{:%FT%TZ}", issued);
    const std::string expires_at =
        request.never_expires ? std::string("never") : std::format("{:%FT%TZ}", expires);
    std::array<std::uint8_t, 32> seed{};
    auto key = generate_key(seed);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto public_value = public_key(key->get());
    if (!public_value) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return std::unexpected(public_value.error());
    }
    auto actor = session_actor(*public_value, issued_at);
    if (!actor) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return std::unexpected(actor.error());
    }
    auto attestation = encode_delegation(
        store, *actor, request.dispatcher_taap_id, request.scopes, issued, identity_options);
    if (!attestation) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return std::unexpected(attestation.error());
    }
    auto encoded_seed = base64(byte_values(seed), true);
    OPENSSL_cleanse(seed.data(), seed.size());
    if (!encoded_seed) {
        return std::unexpected(encoded_seed.error());
    }
    auto encoded_public_key = base64(*public_value, true);
    if (!encoded_public_key) {
        return std::unexpected(encoded_public_key.error());
    }
    NewAgentSession record{
        .id = new_uuid_v4(),
        .customer_id = request.customer_id,
        .durable_agent_taap_id = request.durable_agent_taap_id,
        .dispatcher_taap_id = request.dispatcher_taap_id,
        .session_taap_id = actor->taap_id,
        .public_key = *encoded_public_key,
        .scopes = request.scopes,
        .attestation = *attestation,
        .expires_unix = request.never_expires ? 0 : expires.time_since_epoch().count(),
        .issued_at = issued_at,
        .expires_at = expires_at,
    };
    return MintedSession{std::move(record), std::move(*encoded_seed)};
}

Result<VerifiedDrip> Service::verify_drip(const AgentSession& session,
                                          std::span<const std::byte> canonical_body,
                                          std::span<const std::byte> encoded_signature) const {
    if (!session.revoked_at.empty()) {
        return std::unexpected(Error{"unauthorized", "agent session key has been revoked"});
    }
    if (!session.never_expires() && clock_().time_since_epoch().count() >= session.expires_unix) {
        return std::unexpected(Error{"unauthorized", "agent session key has expired"});
    }
    if (canonical_body.empty() || encoded_signature.empty()) {
        return std::unexpected(Error{"invalid_input", "drip body and signature are required"});
    }
    auto public_value = decode_base64(session.public_key, true);
    if (!public_value || public_value->size() != 32) {
        return std::unexpected(public_value ? Error{"store_error", "stored session key is invalid"}
                                            : public_value.error());
    }
    constexpr std::string_view prefix = "taap:1:ed25519:";
    if (!session.session_taap_id.starts_with(prefix)) {
        return std::unexpected(Error{"store_error", "stored session identity is invalid"});
    }
    const std::string fingerprint = session.session_taap_id.substr(prefix.size());
    const liquid::actor actor{
        session.session_taap_id, "ed25519", fingerprint, std::move(*public_value), fingerprint};
    const auto signature = liquid::decode_signature(encoded_signature);
    if (!signature) {
        return std::unexpected(liquid_error(signature.error()));
    }
    // Lake minted this session key and stored its public half, so the key itself is the binding:
    // check the signature against it directly (liquid#383 made liquid::verify require a chain).
    const auto verified = liquid::verify_key_signature(actor, "drip", canonical_body, *signature);
    if (!verified || !*verified) {
        return std::unexpected(verified ? Error{"unauthorized", "agent drip is unsigned"}
                                        : Error{"unauthorized", verified.error().message});
    }
    return VerifiedDrip{session.durable_agent_taap_id,
                        session.dispatcher_taap_id,
                        session.session_taap_id,
                        session.scopes,
                        session.expires_at};
}

} // namespace lake::control::agents
