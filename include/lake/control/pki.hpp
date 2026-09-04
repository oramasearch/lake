#pragma once

#include <cstddef>
#include <expected>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

struct JwtSvid {
    std::string token;
    std::string issuer;
    std::string subject;
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

Result<Authority> create_authority(std::string_view customer_id);
Result<X509Svid> issue_x509_svid(const Authority& authority,
                                 std::string_view spiffe_id,
                                 int lifetime_seconds = 3600);
Result<void> verify_x509_svid(const Authority& authority, const X509Svid& svid);
Result<JwtSvid> issue_jwt_svid(const Authority& authority,
                               std::string_view spiffe_id,
                               std::string_view audience,
                               int lifetime_seconds = 900);
Result<void> verify_jwt_svid(const Authority& authority, const JwtSvid& svid);

Result<std::vector<ShamirShare>>
split_secret(const std::vector<std::byte>& secret, int threshold, int share_count);
Result<std::vector<std::byte>> recover_secret(const std::vector<ShamirShare>& shares,
                                              int threshold);

class Service {
public:
    Result<AuthorityPublic> create_authority(std::string_view customer_id);
    Result<X509Svid> issue_x509_svid(std::string_view customer_id,
                                     std::string_view spiffe_id,
                                     int lifetime_seconds = 3600);
    Result<JwtSvid> issue_jwt_svid(std::string_view customer_id,
                                   std::string_view spiffe_id,
                                   std::string_view audience,
                                   int lifetime_seconds = 900);
    Result<RecoveryCeremony>
    begin_recovery(std::string_view customer_id, int threshold, int share_count);
    Result<void> recover_root(std::string_view customer_id,
                              const RecoveryCeremony& ceremony,
                              const std::vector<ShamirShare>& shares);

private:
    std::mutex mutex_;
    std::map<std::string, Authority> authorities_;
};

} // namespace lake::control::pki
