// SPDX-License-Identifier: MIT

// mint_sealed_secret and the sealed-secret envelope version 1 (lake#625): a user's workspace secret
// delivered to a Kubernetes Secret the operator writes, sealed so that only the target cluster can
// open it. See include/lake/api/sealed_delivery.hpp and docs/api/secrets.md#projections.

#include "lake/api/sealed_delivery.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "lake/control/store.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace lake::api {
namespace {

using nlohmann::json;

constexpr std::string_view kdf_info = "orama lake sealed secret v1";

// ---- encodings -----------------------------------------------------------------------------

std::string base64_standard(std::span<const std::uint8_t> input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < input.size(); i += 3) {
        const std::uint32_t n = (input[i] << 16) | (input[i + 1] << 8) | input[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == input.size()) {
        const std::uint32_t n = input[i] << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == input.size()) {
        const std::uint32_t n = (input[i] << 16) | (input[i + 1] << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64url_unpadded(std::span<const std::uint8_t> input) {
    std::string out = base64_standard(input);
    std::ranges::replace(out, '+', '-');
    std::ranges::replace(out, '/', '_');
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64url_decode(std::string_view input) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : input) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '-') {
            value = 62;
        } else if (c == '_') {
            value = 63;
        } else {
            return std::nullopt; // padding and the standard alphabet are not accepted
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    if (bits >= 6 || (buffer & ((1u << bits) - 1)) != 0) {
        return std::nullopt;
    }
    return out;
}

// Canonical CBOR (RFC 8949 core deterministic; for these keys the same order as length-first):
// definite lengths, the shortest heads, map keys in the bytewise order of their encodings.
void cbor_head(std::vector<std::uint8_t>& out, std::uint8_t major, std::uint64_t value) {
    const auto type = static_cast<std::uint8_t>(major << 5);
    if (value < 24) {
        out.push_back(type | static_cast<std::uint8_t>(value));
    } else if (value <= 0xff) {
        out.push_back(type | 24);
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(type | 25);
        for (int shift = 8; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    } else if (value <= 0xffffffffULL) {
        out.push_back(type | 26);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    } else {
        out.push_back(type | 27);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
}

std::vector<std::uint8_t> cbor_text(std::string_view text) {
    std::vector<std::uint8_t> out;
    cbor_head(out, 3, text.size());
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

std::vector<std::uint8_t> cbor_bytes(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> out;
    cbor_head(out, 2, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

std::vector<std::uint8_t> cbor_unsigned(std::uint64_t value) {
    std::vector<std::uint8_t> out;
    cbor_head(out, 0, value);
    return out;
}

// A map of text keys to already encoded values, keys ordered by their encodings.
std::vector<std::uint8_t>
cbor_map(std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries) {
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> encoded;
    encoded.reserve(entries.size());
    for (auto& [key, value] : entries) {
        encoded.emplace_back(cbor_text(key), std::move(value));
    }
    std::ranges::sort(encoded, [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::uint8_t> out;
    cbor_head(out, 5, encoded.size());
    for (const auto& [key, value] : encoded) {
        out.insert(out.end(), key.begin(), key.end());
        out.insert(out.end(), value.begin(), value.end());
    }
    return out;
}

// ---- cryptography --------------------------------------------------------------------------

struct PkeyFree {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};
struct PkeyCtxFree {
    void operator()(EVP_PKEY_CTX* ctx) const {
        EVP_PKEY_CTX_free(ctx);
    }
};
struct CipherCtxFree {
    void operator()(EVP_CIPHER_CTX* ctx) const {
        EVP_CIPHER_CTX_free(ctx);
    }
};

// Overwrites key material before it is released.
template <class Container> struct Wipe {
    Container& value;
    ~Wipe() {
        OPENSSL_cleanse(value.data(), value.size());
    }
};

std::expected<std::array<std::uint8_t, 32>, std::string>
x25519_public(const std::array<std::uint8_t, 32>& private_key) {
    std::unique_ptr<EVP_PKEY, PkeyFree> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), 32));
    if (!key) {
        return std::unexpected("X25519 private key rejected");
    }
    std::array<std::uint8_t, 32> out{};
    std::size_t length = out.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), out.data(), &length) != 1 || length != 32) {
        return std::unexpected("X25519 public key derivation failed");
    }
    return out;
}

std::expected<std::array<std::uint8_t, 32>, std::string>
x25519_shared(const std::array<std::uint8_t, 32>& private_key,
              const std::array<std::uint8_t, 32>& peer_public) {
    std::unique_ptr<EVP_PKEY, PkeyFree> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), 32));
    std::unique_ptr<EVP_PKEY, PkeyFree> peer(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public.data(), 32));
    if (!key || !peer) {
        return std::unexpected("X25519 key rejected");
    }
    std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
    std::array<std::uint8_t, 32> shared{};
    std::size_t length = shared.size();
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1 ||
        EVP_PKEY_derive(ctx.get(), shared.data(), &length) != 1 || length != 32) {
        return std::unexpected("X25519 agreement failed");
    }
    if (std::ranges::all_of(shared, [](std::uint8_t b) { return b == 0; })) {
        return std::unexpected("X25519 produced an all-zero shared secret");
    }
    return shared;
}

// HKDF-SHA-256 with a 32-byte zero salt, one 32-byte block of output.
std::array<std::uint8_t, 32> hkdf_sha256(const std::array<std::uint8_t, 32>& shared,
                                         std::span<const std::uint8_t> info) {
    const std::array<std::uint8_t, 32> salt{};
    std::array<std::uint8_t, 32> prk{};
    unsigned int prk_length = 0;
    HMAC(EVP_sha256(),
         salt.data(),
         salt.size(),
         shared.data(),
         shared.size(),
         prk.data(),
         &prk_length);
    std::vector<std::uint8_t> block(info.begin(), info.end());
    block.push_back(1);
    std::array<std::uint8_t, 32> okm{};
    unsigned int okm_length = 0;
    HMAC(EVP_sha256(), prk.data(), prk.size(), block.data(), block.size(), okm.data(), &okm_length);
    OPENSSL_cleanse(prk.data(), prk.size());
    return okm;
}

std::uint32_t load32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

void store32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint32_t rotl(std::uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

// HChaCha20 (draft-irtf-cfrg-xchacha §2.2): the subkey for XChaCha20 from the key and the first
// 16 bytes of the 24-byte nonce.
std::array<std::uint8_t, 32> hchacha20(const std::array<std::uint8_t, 32>& key,
                                       const std::uint8_t* nonce16) {
    std::uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) {
        x[4 + i] = load32(key.data() + 4 * i);
    }
    for (int i = 0; i < 4; ++i) {
        x[12 + i] = load32(nonce16 + 4 * i);
    }
    const auto quarter = [&](int a, int b, int c, int d) {
        x[a] += x[b];
        x[d] = rotl(x[d] ^ x[a], 16);
        x[c] += x[d];
        x[b] = rotl(x[b] ^ x[c], 12);
        x[a] += x[b];
        x[d] = rotl(x[d] ^ x[a], 8);
        x[c] += x[d];
        x[b] = rotl(x[b] ^ x[c], 7);
    };
    for (int round = 0; round < 10; ++round) {
        quarter(0, 4, 8, 12);
        quarter(1, 5, 9, 13);
        quarter(2, 6, 10, 14);
        quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15);
        quarter(1, 6, 11, 12);
        quarter(2, 7, 8, 13);
        quarter(3, 4, 9, 14);
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 4; ++i) {
        store32(out.data() + 4 * i, x[i]);
        store32(out.data() + 16 + 4 * i, x[12 + i]);
    }
    OPENSSL_cleanse(x, sizeof(x));
    return out;
}

// XChaCha20-Poly1305: ChaCha20-Poly1305 (RFC 8439) under the HChaCha20 subkey, with the nonce
// 0x00000000 || nonce[16..24]. Answers ciphertext || 16-byte tag.
std::expected<std::vector<std::uint8_t>, std::string>
xchacha20_poly1305_seal(const std::array<std::uint8_t, 32>& key,
                        const std::array<std::uint8_t, 24>& nonce,
                        std::span<const std::uint8_t> aad,
                        std::string_view plaintext) {
    std::array<std::uint8_t, 32> subkey = hchacha20(key, nonce.data());
    Wipe<std::array<std::uint8_t, 32>> wipe_subkey{subkey};
    std::array<std::uint8_t, 12> iv{};
    std::memcpy(iv.data() + 4, nonce.data() + 16, 8);
    std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree> ctx(EVP_CIPHER_CTX_new());
    std::vector<std::uint8_t> out(plaintext.size() + 16);
    int length = 0;
    int total = 0;
    if (!ctx ||
        EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, subkey.data(), iv.data()) != 1 ||
        EVP_EncryptUpdate(ctx.get(), nullptr, &length, aad.data(), static_cast<int>(aad.size())) !=
            1) {
        return std::unexpected("XChaCha20-Poly1305 setup failed");
    }
    if (!plaintext.empty() &&
        EVP_EncryptUpdate(ctx.get(),
                          out.data(),
                          &length,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        return std::unexpected("XChaCha20-Poly1305 encryption failed");
    }
    total = plaintext.empty() ? 0 : length;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + total, &length) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16, out.data() + plaintext.size()) !=
            1) {
        return std::unexpected("XChaCha20-Poly1305 tag failed");
    }
    return out;
}

// ---- the capability ------------------------------------------------------------------------

ApiResult refused(std::string code, std::string message, std::string path = {}) {
    return ApiResult::failure(std::move(code), std::move(message), std::move(path));
}

std::expected<std::string, ApiError> operator_only(const json&, ApiContext&) {
    return std::unexpected(
        ApiError{"unauthorized", "only the orama-operator delivers secrets", {}});
}

// A Go duration as metav1.Duration serializes it ("1h0m0s", "5m", "300s"), in whole seconds;
// fractions are not accepted.
std::optional<std::int64_t> duration_seconds(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t total = 0;
    while (!text.empty()) {
        std::int64_t number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr == text.data() || number < 0 ||
            number > 1'000'000) {
            return std::nullopt;
        }
        text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
        if (text.starts_with("ms")) {
            text.remove_prefix(2);
            total += number / 1000;
        } else if (text.starts_with("h")) {
            text.remove_prefix(1);
            total += number * 3600;
        } else if (text.starts_with("m")) {
            text.remove_prefix(1);
            total += number * 60;
        } else if (text.starts_with("s")) {
            text.remove_prefix(1);
            total += number;
        } else {
            return std::nullopt;
        }
    }
    return total;
}

// The field values to deliver: every field of the secret's JSON object, a string as itself and
// any other JSON value as its compact text.
std::expected<std::vector<std::pair<std::string, std::string>>, ApiResult>
deliverable_fields(const json& value) {
    if (!value.is_object() || value.empty()) {
        return std::unexpected(
            refused("invalid_input", "the secret has no fields to deliver", "/source"));
    }
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& [key, field] : value.items()) {
        if (!kubernetes_secret_key(key)) {
            return std::unexpected(refused(
                "invalid_input",
                "secret field `" + key + "` cannot be a Kubernetes Secret key ([-._a-zA-Z0-9]+)",
                "/source"));
        }
        fields.emplace_back(key, field.is_string() ? field.get<std::string>() : field.dump());
    }
    return fields;
}

liquid::runtime::task<ApiResult> mint(const json& input, ApiContext& context) {
    if (context.registry == nullptr) {
        co_return refused("store_unavailable", "no registry to read the secret");
    }
    const json& recipient_input = input.at("recipient");
    SealingRecipient recipient;
    recipient.cluster = recipient_input.value("cluster", "");
    recipient.namespace_name = recipient_input.value("namespace", "");
    recipient.name = recipient_input.value("name", "");
    recipient.kid = recipient_input.value("kid", "");
    const std::string public_key_text = recipient_input.value("public_key", "");
    if (recipient.cluster.empty() || recipient.namespace_name.empty() || recipient.name.empty()) {
        co_return refused(
            "invalid_input", "recipient needs cluster, namespace and name", "/recipient");
    }
    auto public_key = decode_sealing_public_key(public_key_text);
    if (!public_key) {
        co_return refused("invalid_input", public_key.error(), "/recipient/public_key");
    }
    recipient.public_key = *public_key;
    if (recipient.kid != sealing_kid(recipient.public_key)) {
        co_return refused(
            "invalid_input", "recipient kid does not name its public key", "/recipient/kid");
    }
    // Lake seals only to the delivery key the operator gave this lake (LAKE_SEALING_PUBLIC_KEY),
    // so a credential that reaches this capability cannot have a value sealed to another key.
    // Without that key nothing is delivered: lake never seals to a key the caller chose
    // (lake#950).
    if (context.sealing_public_key.empty()) {
        co_return refused("unavailable",
                          "this lake has no cluster delivery key (LAKE_SEALING_PUBLIC_KEY)",
                          "/recipient/public_key");
    }
    if (public_key_text != context.sealing_public_key) {
        co_return refused("invalid_input",
                          "recipient is not this lake's cluster delivery key",
                          "/recipient/public_key");
    }

    // The workspace the projection is for, from the app's, service's or store's own row. Every
    // delivery checks the mount belongs to it, not only the write that named the mount
    // (lake#950, lake#971).
    const std::string workspace_id = input.value("workspace_id", "");
    if (workspace_id.empty()) {
        co_return refused(
            "invalid_input", "workspace_id names the workspace the secret is for", "/workspace_id");
    }
    const json& source = input.at("source");
    const std::string type = source.value("type", "");
    const std::string named_mount =
        type == "lake"      ? source.value("lake", json::object()).value("mount", "")
        : type == "dynamic" ? source.value("dynamic", json::object()).value("engine", "")
                            : std::string{};
    if (!named_mount.empty()) {
        if (context.store == nullptr) {
            co_return refused("store_unavailable", "no store to check the secret mount");
        }
        const auto route = context.store->get_secret_mount_route(named_mount);
        if (!route || route->workspace_id != workspace_id) {
            // The same answer for a missing mount and another workspace's: nothing is disclosed.
            co_return refused("not_found",
                              "no such secret mount in workspace " + workspace_id,
                              "/source");
        }
    }
    // The values are read as the operator, the only caller that reaches this handler; every read
    // is recorded in the mount's workspace audit by read_secret / lease_dynamic_secret.
    ApiContext inner = context;
    inner.authorized = true;
    inner.operator_credential = true;
    inner.session_credential = false;
    inner.acting_identity = "admin";
    inner.authentication_method = "sealed_delivery";
    inner.workspace_id.clear();
    inner.path_workspace_id.clear();
    json value;
    json lease = nullptr;
    std::uint64_t version = 0;
    std::string mount_id;
    if (type == "lake") {
        const json lake = source.value("lake", json::object());
        mount_id = lake.value("mount", "");
        const std::string name = lake.value("name", "");
        if (mount_id.empty() || name.empty()) {
            co_return refused(
                "invalid_input", "a lake source needs mount and name", "/source/lake");
        }
        json read_input{{"mount_id", mount_id}, {"name", name}};
        if (lake.contains("version") && !lake.at("version").is_null()) {
            if (!lake.at("version").is_number_integer() ||
                lake.at("version").get<std::int64_t>() < 1) {
                co_return refused(
                    "invalid_input", "version is a positive integer", "/source/lake/version");
            }
            read_input["version"] = lake.at("version");
        }
        auto read = co_await context.registry->call_async("read_secret", read_input, inner);
        if (!read) {
            co_return refused("store_error", read.error().message);
        }
        if (!read->ok) {
            co_return refused(read->error.code, read->error.message, "/source/lake");
        }
        value = read->value.value("value", json::object());
        version = read->value.value("version", std::uint64_t{0});
    } else if (type == "dynamic") {
        // A dynamic source's engine is the secret mount whose role mints the credential.
        const json dynamic = source.value("dynamic", json::object());
        mount_id = dynamic.value("engine", "");
        const std::string role = dynamic.value("role", "");
        const auto ttl = duration_seconds(dynamic.value("ttl", ""));
        if (mount_id.empty() || role.empty() || !ttl || *ttl < 1 || *ttl > 3600) {
            co_return refused("invalid_input",
                              "a dynamic source needs engine (the mount id), role and a ttl of "
                              "1s to 1h",
                              "/source/dynamic");
        }
        auto leased = co_await context.registry->call_async(
            "lease_dynamic_secret",
            {{"mount_id", mount_id}, {"role", role}, {"lifetime_seconds", *ttl}},
            inner);
        if (!leased) {
            co_return refused("store_error", leased.error().message);
        }
        if (!leased->ok) {
            co_return refused(leased->error.code, leased->error.message, "/source/dynamic");
        }
        value = leased->value.value("value", json::object());
        const json& issued = leased->value.at("lease");
        lease = {{"id", issued.value("id", "")}, {"expires_at", issued.value("expires_at", "")}};
    } else {
        co_return refused("invalid_input", "source type must be lake or dynamic", "/source/type");
    }
    auto fields = deliverable_fields(value);
    if (!fields) {
        co_return fields.error();
    }
    json data = json::object();
    for (auto& [key, plaintext] : *fields) {
        auto sealed = seal_for_cluster(recipient, plaintext);
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        if (!sealed) {
            co_return refused("internal", "sealing failed: " + sealed.error());
        }
        data[key] = std::move(*sealed);
    }
    if (context.store != nullptr) {
        if (const auto route = context.store->get_secret_mount_route(mount_id)) {
            context.audit_workspace_id = route->workspace_id;
            context.audit_customer_id = route->customer_id;
        }
    }
    context.audit_kind = "secret_delivery";
    context.audit_object = "secret_mount/" + mount_id;
    context.audit_details["source"] = type;
    context.audit_details["namespace"] = recipient.namespace_name;
    context.audit_details["secret"] = recipient.name;
    context.audit_details["fields"] = data.size();
    if (version != 0) {
        context.audit_details["version"] = version;
    }
    json answer{{"data", std::move(data)}};
    if (version != 0) {
        answer["version"] = version;
    }
    if (!lease.is_null()) {
        answer["lease"] = std::move(lease);
    }
    co_return ApiResult::success(std::move(answer));
}

} // namespace

std::string sealing_kid(const std::array<std::uint8_t, 32>& public_key) {
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(public_key.data(), public_key.size(), digest.data());
    return base64url_unpadded(std::span<const std::uint8_t>(digest.data(), 16));
}

std::expected<std::array<std::uint8_t, 32>, std::string>
decode_sealing_public_key(std::string_view base64url) {
    const auto decoded = base64url_decode(base64url);
    if (!decoded || decoded->size() != 32) {
        return std::unexpected("public_key is not an unpadded base64url 32-byte X25519 key");
    }
    std::array<std::uint8_t, 32> key{};
    std::ranges::copy(*decoded, key.begin());
    return key;
}

std::expected<std::string, std::string> seal_for_cluster(const SealingRecipient& recipient,
                                                         std::string_view plaintext,
                                                         const SealingRandomness* fixed) {
    std::array<std::uint8_t, 32> ephemeral{};
    Wipe<std::array<std::uint8_t, 32>> wipe_ephemeral{ephemeral};
    std::array<std::uint8_t, 24> nonce{};
    if (fixed != nullptr) {
        ephemeral = fixed->ephemeral_private;
        nonce = fixed->nonce;
    } else if (RAND_bytes(ephemeral.data(), static_cast<int>(ephemeral.size())) != 1 ||
               RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return std::unexpected("the system random source failed");
    }
    auto epk = x25519_public(ephemeral);
    if (!epk) {
        return std::unexpected(epk.error());
    }
    auto shared = x25519_shared(ephemeral, recipient.public_key);
    if (!shared) {
        return std::unexpected(shared.error());
    }
    Wipe<std::array<std::uint8_t, 32>> wipe_shared{*shared};
    std::vector<std::uint8_t> info(kdf_info.begin(), kdf_info.end());
    info.insert(info.end(), epk->begin(), epk->end());
    info.insert(info.end(), recipient.public_key.begin(), recipient.public_key.end());
    std::array<std::uint8_t, 32> key = hkdf_sha256(*shared, info);
    Wipe<std::array<std::uint8_t, 32>> wipe_key{key};
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> aad_entries;
    aad_entries.emplace_back("cluster", cbor_text(recipient.cluster));
    aad_entries.emplace_back("namespace", cbor_text(recipient.namespace_name));
    aad_entries.emplace_back("name", cbor_text(recipient.name));
    std::vector<std::uint8_t> aad = cbor_map(aad_entries);
    auto ciphertext = xchacha20_poly1305_seal(key, nonce, aad, plaintext);
    if (!ciphertext) {
        return std::unexpected(ciphertext.error());
    }
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> record;
    record.emplace_back("v", cbor_unsigned(1));
    record.emplace_back("kid", cbor_text(recipient.kid));
    record.emplace_back("epk", cbor_bytes(*epk));
    record.emplace_back("nonce", cbor_bytes(nonce));
    record.emplace_back("aad", std::move(aad));
    record.emplace_back("ct", cbor_bytes(*ciphertext));
    return base64_standard(cbor_map(std::move(record)));
}

bool kubernetes_secret_key(std::string_view key) {
    if (key.empty() || key.size() > 253 || key == "." || key == "..") {
        return false;
    }
    return std::ranges::all_of(key, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
}

std::optional<LakeSecretRef> parse_lake_secret_ref(std::string_view reference) {
    if (!reference.starts_with(lake_secret_scheme)) {
        return std::nullopt;
    }
    reference.remove_prefix(lake_secret_scheme.size());
    const auto slash = reference.find('/');
    const auto hash = reference.rfind('#');
    if (slash == std::string_view::npos || hash == std::string_view::npos || hash <= slash + 1) {
        return std::nullopt;
    }
    LakeSecretRef ref{std::string(reference.substr(0, slash)),
                      std::string(reference.substr(slash + 1, hash - slash - 1)),
                      std::string(reference.substr(hash + 1))};
    if (ref.mount_id.empty() || ref.name.empty() || !kubernetes_secret_key(ref.field)) {
        return std::nullopt;
    }
    return ref;
}

std::string lake_secret_ref(const LakeSecretRef& reference) {
    return std::string(lake_secret_scheme) + reference.mount_id + "/" + reference.name + "#" +
           reference.field;
}

void register_sealed_delivery_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "mint_sealed_secret",
        .title = "Deliver a sealed secret",
        .description =
            "The orama-operator's delivery of a user's workspace secret to the Kubernetes Secret "
            "an app, service or deployment mounts: reads a secret version (a `lake` source) or "
            "leases a credential (a `dynamic` source) and answers each field sealed to the "
            "cluster's delivery key, never a value. Operator credential only.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"attestation", {{"type", "object"}}},
                           {"workspace_id", {{"type", "string"}}},
                           {"recipient",
                            {{"type", "object"},
                             {"properties",
                              {{"cluster", {{"type", "string"}}},
                               {"namespace", {{"type", "string"}}},
                               {"name", {{"type", "string"}}},
                               {"kid", {{"type", "string"}}},
                               {"public_key", {{"type", "string"}}}}},
                             {"required",
                              json::array({"cluster", "namespace", "name", "kid", "public_key"})}}},
                           {"source", {{"type", "object"}}}}},
                         {"required", json::array({"workspace_id", "recipient", "source"})},
                         {"additionalProperties", false}},
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"data", {{"type", "object"}}},
                            {"version", {{"type", "integer"}}},
                            {"lease", {{"type", "object"}}}}}},
        .kind = CapabilityKind::Action,
        .object = "secret_mount",
        .action = "read",
        .resolve_object = operator_only,
        .async_handler = mint});
}

} // namespace lake::api
