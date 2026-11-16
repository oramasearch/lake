#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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

// A root recovery ceremony as begun (lake#934): its quorum and the participants, one share each,
// participant i holding share i + 1. It carries no share; each participant claims their own.
struct RecoveryCeremony {
    std::string id;
    int threshold = 0;
    std::vector<std::string> participants;
    std::string expires_at;
};

// Where a ceremony stands after one participant's share was verified.
struct RecoverySubmission {
    int submitted = 0;
    int threshold = 0;
    bool recovered = false;
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

// An ACME External Account Binding (RFC 8555 §7.3.4, lake#919): what a customer administrator is
// given to enroll an ACME client on that customer. The HMAC key is derived from the instance key,
// the customer and the key id, so nothing is stored, and a key opens no other customer.
struct ExternalAccountBinding {
    std::string key_id;
    std::string hmac_key; // base64url, unpadded
};
Result<ExternalAccountBinding> new_external_account_binding(std::string_view instance_kek,
                                                            std::string_view customer_id);
// Verifies newAccount's `externalAccountBinding` (a flattened JWS, as JSON text): HS256 with the
// derived key of its `kid`, `url` the newAccount URL, no nonce, and the account's own key as its
// payload.
Result<void> verify_external_account_binding(std::string_view instance_kek,
                                             std::string_view customer_id,
                                             std::string_view binding_json,
                                             std::string_view expected_url,
                                             const AcmeJwk& account_jwk);

struct AcmeAuthorization {
    std::string id;
    std::string order_id;
    std::string identifier;
    std::string token;
    std::string key_authorization;
    std::string status;
};

// Bounds on the ACME state a Service keeps in memory (lake#940). Every ACME route is anonymous, so
// nothing an unauthenticated client sends may grow that state without limit.
struct AcmeLimits {
    // How long an issued Replay-Nonce is accepted.
    std::chrono::seconds nonce_lifetime{300};
    // Consumed nonces remembered against replay. Past it the oldest is forgotten and every nonce
    // issued before it is refused.
    std::size_t used_nonces = 16384;
    // An account unused this long is forgotten with its orders.
    std::chrono::seconds account_idle_lifetime{std::chrono::hours(24)};
    // Past either cap the least recently used account (of the customer, or of all) is forgotten.
    std::size_t accounts_per_customer = 16;
    std::size_t accounts = 4096;
    // An order and its authorization are forgotten this long after they were created.
    std::chrono::seconds order_lifetime{std::chrono::hours(1)};
    // Past it the account's oldest order is forgotten.
    std::size_t orders_per_account = 8;
    // The clock every lifetime is measured on; tests replace it.
    std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now;
};

// How many entries the ACME state holds, after expired ones are dropped.
struct AcmeStateSize {
    std::size_t used_nonces = 0;
    std::size_t accounts = 0;
    std::size_t orders = 0;
    std::size_t authorizations = 0;
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
    explicit Service(AcmeLimits acme_limits = {});
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
    // Begin a root recovery ceremony for distinct `participants` (TAAP ids), any `threshold` of
    // whom restore the root. No share is returned: each participant claims only their own.
    Result<RecoveryCeremony> begin_recovery(Store& store,
                                            std::string_view customer_id,
                                            std::string_view instance_kek,
                                            int threshold,
                                            const std::vector<std::string>& participants);
    // The share of `participant` in an active ceremony, issued once.
    Result<ShamirShare> claim_recovery_share(Store& store,
                                             std::string_view customer_id,
                                             std::string_view instance_kek,
                                             std::string_view ceremony_id,
                                             std::string_view participant);
    // Verify the share `participant` presents and record it; the ceremony is recovered once
    // `threshold` distinct participants have presented theirs.
    Result<RecoverySubmission> submit_recovery_share(Store& store,
                                                     std::string_view customer_id,
                                                     std::string_view instance_kek,
                                                     std::string_view ceremony_id,
                                                     std::string_view participant,
                                                     const ShamirShare& share);
    Result<std::string> new_acme_nonce();
    Result<void> consume_acme_nonce(std::string_view nonce);
    // Register an account key for an existing customer.
    Result<AcmeAccount>
    create_acme_account(Store& store, std::string_view customer_id, const AcmeJwk& jwk);
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
    AcmeStateSize acme_state_size();

private:
    using AcmeTime = std::chrono::steady_clock::time_point;
    struct AcmeAccountEntry {
        AcmeAccount account;
        AcmeTime last_used;
        std::vector<std::string> orders; // oldest first
    };
    struct AcmeOrderEntry {
        AcmeOrder order;
        AcmeTime expires_at;
    };

    Result<Authority>
    load_authority(Store& store, std::string_view customer_id, std::string_view instance_kek);
    Result<std::string> acme_nonce_mac(std::uint64_t counter, std::int64_t issued_ms) const;
    void prune_acme_locked(AcmeTime now);
    void touch_acme_account_locked(AcmeAccountEntry& entry, AcmeTime now);
    void remove_acme_account_locked(const std::string& account_id);
    void remove_acme_order_locked(const std::string& order_id);
    // An order of `customer_id` and `account_id` that has not expired, or null.
    AcmeOrderEntry* find_acme_order_locked(std::string_view customer_id,
                                           std::string_view account_id,
                                           std::string_view order_id);

    AcmeLimits acme_limits_;
    std::mutex mutex_;
    std::map<std::string, Authority> authorities_;
    // Nonces are stateless: a counter and issue time under an HMAC with a per-process key. Only
    // consumed ones are kept, by counter; a counter below the floor is refused.
    std::array<unsigned char, 32> acme_nonce_key_{};
    bool acme_nonce_key_ready_ = false;
    std::atomic<std::uint64_t> acme_nonce_counter_{0};
    std::uint64_t acme_nonce_floor_ = 0;
    std::map<std::uint64_t, AcmeTime> acme_used_nonces_;
    std::map<std::string, AcmeAccountEntry> acme_accounts_;
    std::set<std::pair<AcmeTime, std::string>> acme_account_use_;
    std::map<std::string, std::vector<std::string>> acme_customer_accounts_;
    std::map<std::string, AcmeOrderEntry> acme_orders_;
    std::set<std::pair<AcmeTime, std::string>> acme_order_expiry_;
    std::map<std::string, AcmeAuthorization> acme_authorizations_;
    std::map<std::string, std::string> acme_tokens_; // HTTP-01 token -> authorization id
};

} // namespace lake::control::pki
