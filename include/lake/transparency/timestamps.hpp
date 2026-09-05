#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lake/transparency/tuf.hpp"

namespace lake::transparency {

struct TsaReply {
    int status = 0;
    std::string body;
    std::string error;
};

using TsaSender = std::function<TsaReply(std::string_view endpoint, std::string_view request_der)>;

struct TimestampPolicy {
    std::chrono::seconds renewal_interval = std::chrono::days{365};
    std::chrono::seconds renew_before_expiry = std::chrono::days{30};
};

Result<void> validate_timestamp_policy(const TimestampPolicy& policy);

struct ArchiveTimestamp {
    std::uint64_t sequence = 0;
    std::string generated_at;
    std::string policy;
    std::string token_sha256;
};

struct TimestampEvidence {
    std::string checkpoint_digest;
    std::string signature_digest;
    std::string digest_algorithm;
    std::string evidence_record;
    std::vector<ArchiveTimestamp> archive_timestamps;
    std::string renew_after;
};

class TimestampService {
public:
    TimestampService(std::filesystem::path store_root, std::string instance_name);

    Result<TimestampEvidence> timestamp(std::string checkpoint_digest,
                                        std::string_view signature_base64,
                                        std::string_view tsa_endpoint,
                                        std::string_view tsa_trust_pem,
                                        const TimestampPolicy& policy,
                                        const TsaSender& sender);
    Result<TimestampEvidence> renew(std::string_view checkpoint_digest,
                                    std::string_view tsa_endpoint,
                                    std::string_view tsa_trust_pem,
                                    const TimestampPolicy& policy,
                                    const TsaSender& sender);
    Result<TimestampEvidence> evidence(std::string_view checkpoint_digest,
                                       const TimestampPolicy& policy) const;
    Result<std::vector<std::string>> due(std::chrono::system_clock::time_point now,
                                         const TimestampPolicy& policy) const;

    [[nodiscard]] const std::filesystem::path& directory() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::transparency
