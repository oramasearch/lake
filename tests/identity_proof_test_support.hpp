#pragma once
// A person's own liquid identity in tests: its signer, the document `liquid identity show
// --document` prints, and the proof that they hold its key which password sign-up and a first OAuth
// sign-in need with a document (lake#931, docs/api/identity.md#proving-you-hold-the-key).

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <liquid/liquid.hpp>

#include "lake/control/store.hpp"
#include "temporary_directory.hpp"

namespace lake::test {

struct TestIdentity {
    liquid::signer signer;
    std::string document;
};

inline std::string identity_base64url(std::span<const std::byte> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    unsigned buffer = 0;
    int bits = 0;
    for (const std::byte value : bytes) {
        buffer = (buffer << 8U) | std::to_integer<unsigned>(value);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> bits) & 0x3fU]);
        }
    }
    if (bits > 0) {
        output.push_back(alphabet[(buffer << (6 - bits)) & 0x3fU]);
    }
    return output;
}

// A new identity under its own LIQUID_HOME; the previous LIQUID_HOME comes back afterwards. The
// signer keeps its key once the home is gone.
inline TestIdentity test_identity(const std::string& label) {
    const std::filesystem::path home = fixture_path("identity-proof-" + label);
    std::error_code error;
    std::filesystem::remove_all(home, error);
    std::optional<std::string> previous;
    if (const char* value = std::getenv("LIQUID_HOME")) {
        previous = value;
    }
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (previous) {
        setenv("LIQUID_HOME", previous->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    EXPECT_TRUE(signer) << (signer ? "" : signer.error().message);
    auto document = signer->document();
    EXPECT_TRUE(document);
    auto encoded = liquid::encode_identity_document(*document);
    EXPECT_TRUE(encoded);
    std::filesystem::remove_all(home, error);
    return TestIdentity{std::move(*signer), identity_base64url(*encoded)};
}

// What `liquid sign artifact <challenge>` prints: the canonical signature object as hex, and the
// signer (src/cli/sign.cpp in liquid).
inline std::string liquid_sign_artifact(liquid::signer& signer, const std::string& challenge) {
    const auto cbor = nlohmann::json::to_cbor(nlohmann::json(challenge));
    std::vector<std::byte> body(cbor.size());
    std::ranges::transform(cbor, body.begin(), [](std::uint8_t b) { return std::byte{b}; });
    auto signed_value = liquid::runtime::block_on(signer.sign("artifact", std::move(body)));
    EXPECT_TRUE(signed_value);
    auto encoded = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(encoded);
    std::string hex;
    for (const std::byte value : *encoded) {
        static constexpr std::string_view digits = "0123456789abcdef";
        hex.push_back(digits[std::to_integer<unsigned>(value) >> 4U]);
        hex.push_back(digits[std::to_integer<unsigned>(value) & 0x0fU]);
    }
    return "signature=" + hex + "\nsigner=" + signer.identity().taap_id + "\n";
}

// A signature by `identity`'s key over the challenge `store` issues for it in `pool_id`.
inline control::IdentityProof
prove_identity(control::Store& store, const std::string& pool_id, TestIdentity& identity) {
    const auto issued = store.begin_identity_proof(pool_id, identity.document);
    EXPECT_TRUE(issued) << (issued ? "" : issued.error().message);
    if (!issued) {
        return {};
    }
    return {issued->nonce,
            issued->expires_at,
            liquid_sign_artifact(identity.signer, issued->challenge)};
}

// `identity_proof` as sign_up_password and start_oauth take it.
inline nlohmann::json identity_proof_json(const control::IdentityProof& proof) {
    return {
        {"nonce", proof.nonce}, {"expires_at", proof.expires_at}, {"signature", proof.signature}};
}

} // namespace lake::test
