#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

namespace lake::control {

struct PlatformEnvironment {
    std::string id;
    std::string repository_id;
    std::string name;
    std::string object_id;
    std::string created_at;
};

struct PlatformMirrorRow {
    std::string kind;
    std::string key;
    nlohmann::json value;
    std::string seen_at;
};

class PlatformStore {
public:
    virtual ~PlatformStore() = default;
    virtual Result<PlatformEnvironment>
    ensure_platform_environment(const std::string& repository_id, const std::string& name) = 0;
    virtual Result<PlatformEnvironment> get_platform_environment(const std::string& repository_id,
                                                                 const std::string& name) = 0;
    virtual Result<std::vector<PlatformEnvironment>>
    list_platform_environments(const std::string& repository_id) = 0;
    virtual Result<void> upsert_platform_mirror(const PlatformMirrorRow& row) = 0;
    virtual Result<std::vector<PlatformMirrorRow>>
    list_platform_mirror(const std::string& kind) = 0;
};

} // namespace lake::control
