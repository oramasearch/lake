// SPDX-License-Identifier: MIT

#include "lake_attestation.hpp"

#include <cstddef>
#include <span>
#include <string>

#include <openssl/evp.h>

#include "lake/audit/audit.hpp"
#include "lake/control/drive_files.hpp"

namespace lake::api {
namespace {

using nlohmann::json;

std::string base64(std::span<const std::byte> bytes) {
    std::string encoded((bytes.size() + 2) / 3 * 4, '\0');
    if (!bytes.empty()) {
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                        reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<int>(bytes.size()));
    }
    return encoded;
}

} // namespace

json LakeAttestation::to_json() const {
    return {
        {"statement", statement}, {"signer", signer}, {"signature", signature}, {"digest", digest}};
}

std::expected<LakeAttestation, ApiError> sign_lake_attestation(ApiContext& context,
                                                               json statement,
                                                               std::string_view purpose,
                                                               std::string_view what) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                         .instance_kek = context.instance_kek};
    const auto identity = audit::identity(*context.store, options);
    if (!identity) {
        return std::unexpected(ApiError{
            identity.error().code.empty() ? "store_unavailable" : identity.error().code,
            "Lake cannot sign " + std::string(what) + "'s attestation: " + identity.error().message,
            {}});
    }
    LakeAttestation out;
    out.signer = identity->taap_id;
    statement["issuer"] = identity->taap_id;
    out.statement = std::move(statement);
    const std::string body = out.statement.dump();
    const auto signature =
        audit::sign_taap(*context.store,
                         identity->taap_id,
                         std::string(purpose),
                         std::span(reinterpret_cast<const std::byte*>(body.data()), body.size()),
                         options);
    if (!signature) {
        return std::unexpected(
            ApiError{signature.error().code.empty() ? "store_unavailable" : signature.error().code,
                     "Lake could not sign " + std::string(what) +
                         "'s attestation: " + signature.error().message,
                     {}});
    }
    out.signature = base64(*signature);
    const json signed_statement{
        {"statement", out.statement}, {"signer", out.signer}, {"signature", out.signature}};
    out.digest = "sha256:" + control::drive_sha256_hex(signed_statement.dump());
    return out;
}

std::expected<void, ApiError> check_supplied_digest(const json& input,
                                                    const LakeAttestation& attestation,
                                                    std::string_view what) {
    if (!input.contains("attestation_digest") ||
        input.at("attestation_digest").get<std::string>() == attestation.digest) {
        return {};
    }
    return std::unexpected(ApiError{
        "invalid_input",
        "Lake makes and signs " + std::string(what) +
            "'s attestation itself: leave attestation_digest out. A digest is accepted only when "
            "it is the one Lake answered for this same change.",
        "/attestation_digest"});
}

} // namespace lake::api
