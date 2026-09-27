#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

// Running user-created and library agents (docs/api/agent-launches.md, lake#231): the scopes a
// launch's credential carries, when a trigger is next due, the scheduler that fires schedules, and
// the injected caller that sends a turn to an agent's service.
namespace lake::control {

// The lake capabilities a launched agent's credential may call: its workspace through the agent
// path (lake#235), and the repository reads a definition stored in lake needs. Its skills narrow
// further inside agent-runtime.
inline const std::vector<std::string> agent_launch_scopes{"workspace_query",
                                                          "workspace_act",
                                                          "workspace_openapi",
                                                          "list_agent_library",
                                                          "repository_tree",
                                                          "repository_file"};

// When `trigger` (Studio's plain words) is next due after `now_unix`, or nothing when lake cannot
// schedule it by time. Understood: "every N minutes|hours|days", "hourly", "daily", "weekly",
// "every day at HH:MM", "every <weekday> at HH:MM", "every weekday at HH:MM" (UTC).
std::optional<std::int64_t> next_run(std::string_view trigger, std::int64_t now_unix);

// Fires due schedules as job launches, acting as whoever set the schedule.
class AgentScheduler {
public:
    AgentScheduler(std::shared_ptr<Store> store, std::chrono::seconds interval);
    ~AgentScheduler();
    AgentScheduler(const AgentScheduler&) = delete;
    AgentScheduler& operator=(const AgentScheduler&) = delete;

    void start();
    // One pass: launch every due schedule and move it to its next run. Returns how many fired.
    std::size_t tick(std::int64_t now_unix);

private:
    std::shared_ptr<Store> store_;
    std::chrono::seconds interval_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// One turn to an agent's service: POST <endpoint>/v1/turn with the service token as bearer.
// Injected so tests need no HTTP server.
class AgentCaller {
public:
    virtual ~AgentCaller() = default;
    virtual liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    turn(std::string endpoint, std::string service_token, nlohmann::json body) = 0;
};

class HttpAgentCaller final : public AgentCaller {
public:
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    turn(std::string endpoint, std::string service_token, nlohmann::json body) override;
};

} // namespace lake::control
