#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control::callers {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct TokenReview {
    bool authenticated = false;
    std::string namespace_name;
    std::string service_account;
    std::vector<std::string> audiences;
};

struct VerifiedJwt {
    std::string issuer;
    std::string subject;
    nlohmann::json claims = nlohmann::json::object();
};

struct VerifiedCloudIdentity {
    std::string provider;
    std::string subject;
    nlohmann::json attributes = nlohmann::json::object();
};

struct Credential {
    std::string value;
    std::string workload_taap_id;
    std::string attestation_type;
    std::string subject;
    std::string expires_at;
};

class TokenReviewer {
public:
    virtual ~TokenReviewer() = default;
    virtual Result<TokenReview> review(std::string_view token,
                                       const std::vector<std::string>& audiences) = 0;
};

class JwtVerifier {
public:
    virtual ~JwtVerifier() = default;
    virtual Result<VerifiedJwt> verify(std::string_view token) = 0;
};

class CloudIdentityVerifier {
public:
    virtual ~CloudIdentityVerifier() = default;
    virtual Result<VerifiedCloudIdentity> verify(std::string_view provider,
                                                 std::string_view document) = 0;
};

// This matches the SSO branch's federation_issuers policy lookup. Its storage implementation
// moves to Store when issue-15-sso-federation lands; callers depend only on this narrow contract.
class FederationIssuerPolicy {
public:
    virtual ~FederationIssuerPolicy() = default;
    virtual Result<bool>
    allows(std::string_view organization_id, std::string_view issuer, std::string_view claim) = 0;
};

class InMemoryFederationIssuerPolicy final : public FederationIssuerPolicy {
public:
    void allow(std::string organization_id, std::string issuer, std::string claim);
    Result<bool> allows(std::string_view organization_id,
                        std::string_view issuer,
                        std::string_view claim) override;

private:
    std::map<std::string, bool> entries_;
};

class Service {
public:
    Service(TokenReviewer* token_reviewer,
            JwtVerifier* jwt_verifier,
            CloudIdentityVerifier* cloud_verifier,
            FederationIssuerPolicy* issuer_policy);

    Result<Credential> exchange_kubernetes(std::string_view organization_id,
                                           std::string_view token,
                                           std::string_view workload_taap_id,
                                           const std::vector<std::string>& audiences,
                                           int lifetime_seconds = 900);
    Result<Credential> exchange_jwt(std::string_view organization_id,
                                    std::string_view token,
                                    std::string_view workload_taap_id,
                                    int lifetime_seconds = 900);
    Result<Credential> exchange_cloud(std::string_view provider,
                                      std::string_view document,
                                      std::string_view workload_taap_id,
                                      int lifetime_seconds = 900);

private:
    Result<Credential> issue(std::string_view workload_taap_id,
                             std::string_view attestation_type,
                             std::string_view subject,
                             int lifetime_seconds);

    TokenReviewer* token_reviewer_;
    JwtVerifier* jwt_verifier_;
    CloudIdentityVerifier* cloud_verifier_;
    FederationIssuerPolicy* issuer_policy_;
};

} // namespace lake::control::callers
