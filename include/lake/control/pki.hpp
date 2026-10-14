#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lake::control {
class Store;
struct NetworkTunnelGrant;
}

namespace lake::control::pki {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct Authority {
    std::string root_certificate_pem;
    std::string intermediate_certificate_pem;
    std::string root_private_key_pem;
    std::string intermediate_private_key_pem;
};

struct X509Svid {
    std::string certificate_pem;
    std::string private_key_pem;
    std::string chain_pem;
};

struct SignedClientCertificate {
    std::string certificate_pem;
    std::string chain_pem;
    std::chrono::system_clock::time_point expires_at;
};

struct JwtSvid {
    std::string token;
    std::string issuer;
    std::string subject;
};

struct SshCertificate {
    std::string certificate;
    std::string ca_public_key;
    std::uint64_t serial = 0;
};

struct ShamirShare {
    int index = 0;
    std::vector<std::byte> value;
};

struct AuthorityPublic {
    std::string root_certificate_pem;
    std::string intermediate_chain_pem;
};

struct RecoveryCeremony {
    std::string id;
    int threshold = 0;
    std::vector<ShamirShare> shares;
};

struct AcmeOrder {
    std::string id;
    std::string customer_id;
    std::string account_id;
    std::string identifier;
    std::string authorization_id;
    std::string status;
    std::string certificate_pem;
};

struct AcmeJwk {
    std::string x;
    std::string y;
};

struct AcmeAccount {
    std::string id;
    std::string customer_id;
    AcmeJwk jwk;
};

struct AcmeAuthorization {
    std::string id;
    std::string order_id;
    std::string identifier;
    std::string token;
    std::string key_authorization;
    std::string status;
};

Result<Authority> create_authority(std::string_view customer_id);
Result<X509Svid> issue_x509_svid(const Authority& authority,
                                 std::string_view spiffe_id,
                                 int lifetime_seconds = 3600);
Result<X509Svid> issue_x509_certificate(const Authority& authority,
                                        std::string_view subject,
                                        int lifetime_seconds = 3600);
Result<X509Svid> issue_agent_certificate(const Authority& authority,
                                         std::string_view subject,
                                         std::string_view public_key,
                                         int lifetime_seconds = 3600);
Result<void> verify_x509_svid(const Authority& authority, const X509Svid& svid);
Result<JwtSvid> issue_jwt_svid(const Authority& authority,
                               std::string_view spiffe_id,
                               std::string_view audience,
                               int lifetime_seconds = 900);
Result<void> verify_jwt_svid(const Authority& authority, const JwtSvid& svid);
Result<SshCertificate> issue_ssh_certificate(const Authority& authority,
                                             std::string_view public_key,
                                             std::string_view key_id,
                                             const std::vector<std::string>& principals,
                                             bool host,
                                             int lifetime_seconds = 3600);

Result<std::vector<ShamirShare>>
split_secret(const std::vector<std::byte>& secret, int threshold, int share_count);
Result<std::vector<std::byte>> recover_secret(const std::vector<ShamirShare>& shares,
                                              int threshold);

class Service {
public:
    Result<AuthorityPublic> create_authority(std::string_view customer_id);
    Result<AuthorityPublic>
    create_authority(Store& store, std::string_view customer_id, std::string_view instance_kek);
    // Load the durable customer CA, creating it only when absent. Concurrent creation is safe.
    Result<AuthorityPublic>
    ensure_authority(Store& store, std::string_view customer_id, std::string_view instance_kek);
    // Verify a PKCS#10 CSR and sign its public key only. Requested subject/extensions are ignored.
    Result<SignedClientCertificate> sign_tunnel_certificate(Store& store,
                                                            const NetworkTunnelGrant& grant,
                                                            std::string_view csr_pem,
                                                            std::string_view instance_kek);
    Result<X509Svid> issue_x509_svid(std::string_view customer_id,
                                     std::string_view spiffe_id,
                                     int lifetime_seconds = 3600);
    Result<X509Svid> issue_x509_svid(Store& store,
                                     std::string_view customer_id,
                                     std::string_view spiffe_id,
                                     std::string_view instance_kek,
                                     int lifetime_seconds = 3600);
    Result<X509Svid> issue_x509_certificate(Store& store,
                                            std::string_view customer_id,
                                            std::string_view subject,
                                            std::string_view instance_kek,
                                            int lifetime_seconds = 3600);
    Result<X509Svid> issue_agent_certificate(Store& store,
                                             std::string_view customer_id,
                                             std::string_view subject,
                                             std::string_view public_key,
                                             std::string_view instance_kek,
                                             int lifetime_seconds = 3600);
    Result<JwtSvid> issue_jwt_svid(std::string_view customer_id,
                                   std::string_view spiffe_id,
                                   std::string_view audience,
                                   int lifetime_seconds = 900);
    Result<JwtSvid> issue_jwt_svid(Store& store,
                                   std::string_view customer_id,
                                   std::string_view spiffe_id,
                                   std::string_view audience,
                                   std::string_view instance_kek,
                                   int lifetime_seconds = 900);
    Result<SshCertificate> issue_ssh_certificate(Store& store,
                                                 std::string_view customer_id,
                                                 std::string_view public_key,
                                                 std::string_view key_id,
                                                 const std::vector<std::string>& principals,
                                                 bool host,
                                                 std::string_view instance_kek,
                                                 int lifetime_seconds = 3600);
    Result<RecoveryCeremony>
    begin_recovery(std::string_view customer_id, int threshold, int share_count);
    Result<RecoveryCeremony> begin_recovery(Store& store,
                                            std::string_view customer_id,
                                            std::string_view instance_kek,
                                            int threshold,
                                            int share_count);
    Result<void> recover_root(std::string_view customer_id,
                              const RecoveryCeremony& ceremony,
                              const std::vector<ShamirShare>& shares);
    Result<void> recover_root(Store& store,
                              std::string_view customer_id,
                              std::string_view instance_kek,
                              const RecoveryCeremony& ceremony,
                              const std::vector<ShamirShare>& shares);
    Result<std::string> new_acme_nonce();
    Result<void> consume_acme_nonce(std::string_view nonce);
    Result<AcmeAccount> create_acme_account(std::string_view customer_id, const AcmeJwk& jwk);
    Result<void> verify_acme_signature(std::string_view account_id,
                                       const std::optional<AcmeJwk>& jwk,
                                       std::string_view signing_input,
                                       std::string_view signature);
    Result<AcmeOrder> create_acme_order(Store& store,
                                        std::string_view customer_id,
                                        std::string_view account_id,
                                        std::string_view identifier,
                                        std::string_view instance_kek);
    Result<AcmeOrder> acme_order(std::string_view customer_id,
                                 std::string_view account_id,
                                 std::string_view order_id);
    Result<AcmeAuthorization> acme_authorization(std::string_view customer_id,
                                                 std::string_view account_id,
                                                 std::string_view authorization_id);
    Result<AcmeAuthorization> authorize_acme_order(std::string_view customer_id,
                                                   std::string_view account_id,
                                                   std::string_view authorization_id);
    Result<std::string> acme_http01_response(std::string_view token);
    Result<AcmeOrder> finalize_acme_order(Store& store,
                                          std::string_view customer_id,
                                          std::string_view account_id,
                                          std::string_view order_id,
                                          std::string_view csr,
                                          std::string_view instance_kek);

private:
    Result<Authority>
    load_authority(Store& store, std::string_view customer_id, std::string_view instance_kek);
    std::mutex mutex_;
    std::map<std::string, Authority> authorities_;
    std::set<std::string> acme_nonces_;
    std::map<std::string, AcmeAccount> acme_accounts_;
    std::map<std::string, AcmeOrder> acme_orders_;
    std::map<std::string, AcmeAuthorization> acme_authorizations_;
};

} // namespace lake::control::pki
