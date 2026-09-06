#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/platform_store.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

struct PlatformError {
    std::string code;
    std::string message;
};

template <class T> using PlatformResult = std::expected<T, PlatformError>;

class LokiSource {
public:
    virtual ~LokiSource() = default;
    virtual liquid::runtime::task<PlatformResult<nlohmann::json>>
    query(std::string namespace_name, std::string since, int limit, std::string direction) = 0;
};

class LokiHttpSource final : public LokiSource {
public:
    LokiHttpSource(std::string endpoint, std::string bearer_token);
    ~LokiHttpSource() override;
    LokiHttpSource(LokiHttpSource&&) noexcept;
    LokiHttpSource& operator=(LokiHttpSource&&) noexcept;
    LokiHttpSource(const LokiHttpSource&) = delete;
    LokiHttpSource& operator=(const LokiHttpSource&) = delete;

    liquid::runtime::task<PlatformResult<nlohmann::json>>
    query(std::string namespace_name, std::string since, int limit, std::string direction) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PlatformConfig {
    std::string lake;
    std::chrono::seconds sync_interval{15};
    std::string identity_seed;
    std::string instance_kek;
};

class PlatformService {
public:
    PlatformService(std::shared_ptr<Store> store,
                    std::shared_ptr<ZettaControl> control,
                    std::shared_ptr<LokiSource> logs,
                    PlatformConfig config);

    liquid::runtime::task<PlatformResult<void>> sync();
    [[nodiscard]] bool sync_due() const;
    [[nodiscard]] bool degraded() const;
    [[nodiscard]] std::optional<std::string> stale_since() const;

    PlatformResult<nlohmann::json> list_environments(const std::string& repository_id) const;
    PlatformResult<nlohmann::json> get_environment(const std::string& repository_id,
                                                   const std::string& environment) const;
    PlatformResult<nlohmann::json> list_deployments(const std::string& repository_id,
                                                    const std::string& environment,
                                                    int limit) const;
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    environment_logs(std::string repository_id,
                     std::string environment,
                     std::string since,
                     int limit,
                     std::string direction);
    liquid::runtime::task<PlatformResult<void>> set_environment_env(std::string repository_id,
                                                                    std::string environment,
                                                                    std::string key,
                                                                    std::string value,
                                                                    bool deleted,
                                                                    std::string actor);
    liquid::runtime::task<PlatformResult<void>> change_domain(std::string repository_id,
                                                              std::string environment,
                                                              std::string host,
                                                              std::string tls,
                                                              bool deleted,
                                                              std::string actor);

private:
    PlatformResult<void> require_writable() const;
    PlatformResult<nlohmann::json> environment_document(const std::string& repository_id,
                                                        const std::string& environment) const;
    void mark_failed();
    void mark_succeeded();

    std::shared_ptr<Store> store_;
    PlatformStore* platform_store_ = nullptr;
    std::shared_ptr<ZettaControl> control_;
    std::shared_ptr<LokiSource> logs_;
    PlatformConfig config_;
    mutable std::mutex state_mutex_;
    bool degraded_ = false;
    std::optional<std::string> stale_since_;
    std::chrono::steady_clock::time_point next_sync_ = std::chrono::steady_clock::now();
};

} // namespace lake::control
