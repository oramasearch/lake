#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/transparency/tuf.hpp"

namespace lake::transparency {

enum class LogMode {
    AirGapped,
    Witnessed,
    Anchored,
};

struct Witness {
    std::string endpoint;
    std::string public_key;
};

struct LogConfiguration {
    LogMode mode = LogMode::AirGapped;
    std::vector<Witness> witnesses;
    std::size_t witness_threshold = 0;
    std::string aggregator_endpoint;
};

std::string_view to_string(LogMode mode);
Result<LogConfiguration> log_configuration_from_environment(const std::string& mode,
                                                            const std::string& witnesses,
                                                            std::uint64_t threshold,
                                                            const std::string& aggregator_endpoint);
Result<void> validate_log_configuration(const LogConfiguration& configuration);
nlohmann::json log_configuration_json(const LogConfiguration& configuration);

using CheckpointSigner = std::function<Result<nlohmann::json>(const nlohmann::json& checkpoint)>;
using TransparencySubmitter = std::function<Result<nlohmann::json>(const std::string& endpoint,
                                                                   const nlohmann::json& request)>;

class CheckpointService {
public:
    CheckpointService(std::filesystem::path store_root,
                      std::string instance_name,
                      LogConfiguration configuration);

    Result<nlohmann::json> publish(const nlohmann::json& checkpoint,
                                   const CheckpointSigner& signer,
                                   const TransparencySubmitter& submitter);
    Result<nlohmann::json> adopt(const nlohmann::json& record);
    Result<nlohmann::json> current() const;
    Result<nlohmann::json> at_tree_size(std::uint64_t tree_size) const;
    [[nodiscard]] nlohmann::json configuration() const;
    [[nodiscard]] const std::filesystem::path& directory() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::transparency
