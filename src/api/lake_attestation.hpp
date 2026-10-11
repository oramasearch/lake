#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"

namespace lake::api {

// A statement lake signs itself as the evidence of an access change (a grant, a membership, a
// shared folder), so the `attestation_digest` it keeps proves something (lake#585, lake#602).
// `statement` gets `issuer` (lake's TAAP identity); `signature` is lake's Ed25519 signature over
// the statement's JSON, base64; `digest` is `sha256:<hex>` of the JSON of
// `{statement, signer, signature}`. The statement has no time and Ed25519 is deterministic, so the
// same change under the same lake identity always has the same digest.
struct LakeAttestation {
    nlohmann::json statement;
    std::string signer;
    std::string signature;
    std::string digest;

    // `{statement, signer, signature, digest}`, as answers and audit events carry it.
    [[nodiscard]] nlohmann::json to_json() const;
};

// Signs `statement` with lake's identity under `purpose` (the TAAP signing purpose). `what` names
// the change in an error ("the share", "the grant").
std::expected<LakeAttestation, ApiError> sign_lake_attestation(ApiContext& context,
                                                               nlohmann::json statement,
                                                               std::string_view purpose,
                                                               std::string_view what);

// The answer to a caller's `attestation_digest`: absent or equal to lake's digest is fine; anything
// else is `invalid_input` at `/attestation_digest`, saying lake makes the attestation.
std::expected<void, ApiError> check_supplied_digest(const nlohmann::json& input,
                                                    const LakeAttestation& attestation,
                                                    std::string_view what);

} // namespace lake::api
