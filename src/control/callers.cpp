#include "lake/control/callers.hpp"

#include "lake/control/store.hpp"

#include <array>
#include <chrono>
#include <format>
#include <utility>

#include <openssl/rand.h>

namespace lake::control::callers {
namespace {

std::string
policy_key(std::string_view organization_id, std::string_view issuer, std::string_view claim) {
    return std::string(organization_id) + "\n" + std::string(issuer) + "\n" + std::string(claim);
}

Result<void> validate_workload(std::string_view workload_taap_id, int lifetime_seconds) {
    if (!workload_taap_id.starts_with("taap:1:")) {
        return std::unexpected(Error{"invalid_input", "workload TAAP identity is required"});
    }
    if (lifetime_seconds <= 0 || lifetime_seconds > 900) {
        return std::unexpected(
            Error{"invalid_input", "credential lifetime must be at most fifteen minutes"});
    }
    return {};
}

Result<std::string> base64url_encode(const std::array<unsigned char, 32>& value) {
    std::string encoded(4 * ((value.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                        value.data(),
                                        static_cast<int>(value.size()));
    if (written < 0) {
        return std::unexpected(Error{"crypto_error", "could not encode workload credential"});
    }
    encoded.resize(static_cast<std::size_t>(written));
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    for (char& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    return encoded;
}

} // namespace

void InMemoryFederationIssuerPolicy::allow(std::string organization_id,
                                           std::string issuer,
                                           std::string claim) {
    entries_[policy_key(organization_id, issuer, claim)] = true;
}

Result<bool> InMemoryFederationIssuerPolicy::allows(std::string_view organization_id,
                                                    std::string_view issuer,
                                                    std::string_view claim) {
    return entries_.contains(policy_key(organization_id, issuer, claim));
}

StoreFederationIssuerPolicy::StoreFederationIssuerPolicy(Store* store) : store_(store) {}

Result<bool> StoreFederationIssuerPolicy::allows(std::string_view organization_id,
                                                 std::string_view issuer,
                                                 std::string_view claim) {
    if (!store_) {
        return std::unexpected(Error{"store_unavailable", "federation store is unavailable"});
    }
    auto allowed = store_->allows_federation_claim(
        std::string(organization_id), std::string(issuer), std::string(claim));
    if (!allowed) {
        return std::unexpected(Error{allowed.error().code, allowed.error().message});
    }
    return *allowed;
}

Service::Service(TokenReviewer* token_reviewer,
                 JwtVerifier* jwt_verifier,
                 CloudIdentityVerifier* cloud_verifier,
                 FederationIssuerPolicy* issuer_policy)
    : token_reviewer_(token_reviewer), jwt_verifier_(jwt_verifier), cloud_verifier_(cloud_verifier),
      issuer_policy_(issuer_policy) {}

Result<Credential> Service::exchange_kubernetes(std::string_view organization_id,
                                                std::string_view token,
                                                std::string_view workload_taap_id,
                                                const std::vector<std::string>& audiences,
                                                int lifetime_seconds) {
    auto valid = validate_workload(workload_taap_id, lifetime_seconds);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (!token_reviewer_ || organization_id.empty() || token.empty() || audiences.empty()) {
        return std::unexpected(
            Error{"invalid_input", "TokenReview configuration and token are required"});
    }
    auto review = token_reviewer_->review(token, audiences);
    if (!review) {
        return std::unexpected(review.error());
    }
    if (!review->authenticated || review->namespace_name.empty() ||
        review->service_account.empty()) {
        return std::unexpected(
            Error{"unauthorized", "Kubernetes TokenReview did not authenticate the caller"});
    }
    const std::string subject =
        "system:serviceaccount:" + review->namespace_name + ":" + review->service_account;
    return issue(workload_taap_id,
                 "kubernetes",
                 "kubernetes",
                 subject,
                 {{"namespace", review->namespace_name},
                  {"service_account", review->service_account},
                  {"audiences", review->audiences}},
                 lifetime_seconds);
}

Result<Credential> Service::exchange_jwt(std::string_view organization_id,
                                         std::string_view token,
                                         std::string_view workload_taap_id,
                                         int lifetime_seconds) {
    auto valid = validate_workload(workload_taap_id, lifetime_seconds);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (!jwt_verifier_ || !issuer_policy_ || organization_id.empty() || token.empty()) {
        return std::unexpected(
            Error{"invalid_input", "issuer policy and JWT verifier are required"});
    }
    auto assertion = jwt_verifier_->verify(token);
    if (!assertion) {
        return std::unexpected(assertion.error());
    }
    if (assertion->issuer.empty() || assertion->subject.empty() || !assertion->claims.is_object()) {
        return std::unexpected(Error{"unauthorized", "JWT attestation claims are invalid"});
    }
    for (auto it = assertion->claims.begin(); it != assertion->claims.end(); ++it) {
        auto allowed = issuer_policy_->allows(organization_id, assertion->issuer, it.key());
        if (!allowed) {
            return std::unexpected(allowed.error());
        }
        if (!*allowed) {
            return std::unexpected(
                Error{"unauthorized", "issuer may not assert claim " + it.key()});
        }
    }
    return issue(workload_taap_id,
                 "jwt",
                 assertion->issuer,
                 assertion->subject,
                 assertion->claims,
                 lifetime_seconds);
}

Result<Credential> Service::exchange_cloud(std::string_view provider,
                                           std::string_view document,
                                           std::string_view workload_taap_id,
                                           int lifetime_seconds) {
    auto valid = validate_workload(workload_taap_id, lifetime_seconds);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    if (!cloud_verifier_ || (provider != "aws" && provider != "azure") || document.empty()) {
        return std::unexpected(
            Error{"invalid_input", "AWS or Azure identity evidence is required"});
    }
    auto identity = cloud_verifier_->verify(provider, document);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    if (identity->provider != provider || identity->subject.empty()) {
        return std::unexpected(Error{"unauthorized", "cloud identity evidence is invalid"});
    }
    return issue(workload_taap_id,
                 provider,
                 provider,
                 identity->subject,
                 identity->attributes,
                 lifetime_seconds);
}

Result<Credential> Service::issue(std::string_view workload_taap_id,
                                  std::string_view attestation_type,
                                  std::string_view issuer,
                                  std::string_view subject,
                                  nlohmann::json attributes,
                                  int lifetime_seconds) {
    if (attributes.is_null()) {
        attributes = nlohmann::json::object();
    }
    if (!attributes.is_object()) {
        return std::unexpected(
            Error{"invalid_input", "verified caller attributes must be an object"});
    }
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return std::unexpected(Error{"crypto_error", "could not create workload credential"});
    }
    auto value = base64url_encode(bytes);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto expires_at =
        std::chrono::system_clock::now() + std::chrono::seconds(lifetime_seconds);
    return Credential{
        *value,
        std::string(workload_taap_id),
        std::string(attestation_type),
        std::string(issuer),
        std::string(subject),
        std::move(attributes),
        std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(expires_at))};
}

} // namespace lake::control::callers
