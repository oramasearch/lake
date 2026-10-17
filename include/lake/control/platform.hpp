#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"
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
    // Every this many polls the sync rereads every repository's workspace instead of trusting
    // what it wrote last (one in-process cache per replica).
    int full_sync_every = 20;
};

// Opens the records of one workspace of one space (the API layer binds it to the workspace
// caller). The sync uses it to reach each repository's workspace.
using PlatformRecordsOpener = std::function<ZettaControlResult<WorkspaceRecords>(
    const std::string& customer_id, const std::string& workspace_id)>;

// Reads the id liquid gave a repository (32 lowercase hex digits; the API layer opens the
// repository's store). The sync uses it to record the ids lake has not read yet when a row names
// one it does not know (lake#805, docs/api/platform.md#which-repository-a-row-names).
using PlatformLiquidIdReader = std::function<PlatformResult<std::string>(const Repository&)>;

// Lake's view of the operator's deployment control plane (docs/api/platform.md). The sync reads
// the operator-control rows and keeps each repository's environments and last-seen rows in the
// workspace Zetta of the repository's workspace (lake#358, lake#359); every read and write takes
// that workspace's records, which the caller has opened for the repository.
class PlatformService {
public:
    PlatformService(std::shared_ptr<Store> store,
                    std::shared_ptr<ZettaControl> control,
                    std::shared_ptr<LokiSource> logs,
                    PlatformConfig config);

    // One poll. Failing to read the operator control plane degrades the whole service; failing to
    // reach one repository's workspace marks only that repository stale and is retried next poll.
    // A row's `repository` is lake's repository id or the id liquid gave it (what `liquid deploy`
    // sends); a row lake cannot place in one repository is reported, never dropped quietly: the
    // poll answers `unresolved_repository` naming it, and unresolved_repositories() lists it.
    liquid::runtime::task<PlatformResult<void>> sync(PlatformRecordsOpener open,
                                                     PlatformLiquidIdReader read_liquid_id = {});
    [[nodiscard]] bool sync_due() const;
    [[nodiscard]] bool degraded() const;
    [[nodiscard]] std::optional<std::string> stale_since() const;
    // Since when this repository's mirror has not been refreshed: the service's own stale marker,
    // else the repository's.
    [[nodiscard]] std::optional<std::string> stale_since(const std::string& repository_id) const;
    // The `repository` values of the last poll's rows that name no single lake repository, each
    // with the reason and since when (lake#805).
    struct UnresolvedRepository {
        std::string reason;
        std::string since;
    };
    [[nodiscard]] std::map<std::string, UnresolvedRepository> unresolved_repositories() const;

    liquid::runtime::task<PlatformResult<nlohmann::json>>
    list_environments(WorkspaceRecords& records, std::string repository_id) const;
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    get_environment(WorkspaceRecords& records, std::string repository_id,
                    std::string environment) const;
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    list_deployments(WorkspaceRecords& records, std::string repository_id,
                     std::string environment, int limit) const;
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    environment_logs(WorkspaceRecords& records,
                     std::string repository_id,
                     std::string environment,
                     std::string since,
                     int limit,
                     std::string direction);
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    app_logs(std::string app_id, std::string since, int limit, std::string direction);
    liquid::runtime::task<PlatformResult<void>> set_environment_env(WorkspaceRecords& records,
                                                                    std::string repository_id,
                                                                    std::string environment,
                                                                    std::string key,
                                                                    std::string value,
                                                                    bool deleted,
                                                                    std::string actor);
    liquid::runtime::task<PlatformResult<void>> change_domain(WorkspaceRecords& records,
                                                              std::string repository_id,
                                                              std::string environment,
                                                              std::string host,
                                                              std::string tls,
                                                              bool deleted,
                                                              std::string actor);

private:
    PlatformResult<void> require_writable() const;
    liquid::runtime::task<PlatformResult<nlohmann::json>>
    environment_document(WorkspaceRecords& records, std::string repository_id,
                         std::string environment) const;
    // The environment's policy object (created on first sight) and its workspace record.
    liquid::runtime::task<PlatformResult<PlatformEnvironment>>
    ensure_environment(WorkspaceRecords& records, const Repository& repository,
                       const std::string& name);
    void mark_failed();
    void mark_succeeded();
    void mark_repository(const std::string& repository_id, bool mirrored);

    std::shared_ptr<Store> store_;
    std::shared_ptr<ZettaControl> control_;
    std::shared_ptr<LokiSource> logs_;
    PlatformConfig config_;
    mutable std::mutex state_mutex_;
    bool degraded_ = false;
    std::optional<std::string> stale_since_;
    std::chrono::steady_clock::time_point next_sync_ = std::chrono::steady_clock::now();
    // Repositories whose workspace could not be written, and since when.
    std::map<std::string, std::string> stale_repositories_;
    // What the last successful sync wrote per repository, so an unchanged repository costs no
    // workspace call; forgotten every `full_sync_every` polls, so a workspace that lost its
    // records (recreated) is written again.
    std::map<std::string, std::string> mirrored_;
    int syncs_since_full_ = 0;
    std::map<std::string, UnresolvedRepository> unresolved_;
    // Polls since lake last read the liquid ids it lacked; read again at most every
    // `full_sync_every` polls while some row stays unresolved (-1: never read).
    int syncs_since_liquid_ids_ = -1;
};

} // namespace lake::control
