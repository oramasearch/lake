#pragma once

#include <chrono>
#include <expected>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

// Running user-created and library agents (docs/api/agent-launches.md, lake#231): the scopes a
// launch's credential carries, the cron expressions a schedule may use (the Orama operator runs
// each schedule as a CronJob), and the injected caller that sends a turn to an agent's service.
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

// A schedule's cron expression (docs/api/agent-launches.md): the five fields a Kubernetes CronJob
// takes (minute, hour, day of month, month, day of week), or one of @hourly, @daily, @weekly,
// @monthly, @yearly. Returns the five-field form, or why lake refuses it. A schedule may fire at
// most once every five minutes.
std::expected<std::string, std::string> normalize_cron(std::string_view expression);

// Studio's plain-words trigger as a cron expression, or why lake cannot place it in time.
// Understood: "every N minutes|hours|days", "hourly", "daily", "weekly", "every day at HH:MM",
// "every weekday at HH:MM", "every <weekday> at HH:MM". A trigger more often than once every five
// minutes says so (lake#442).
std::expected<std::string, std::string> trigger_cron(std::string_view trigger);

// How long a launch may wait for the Orama operator to report on it before lake withdraws it, and
// the reason it records (lake#442). Lake's minute loop applies it; reads apply it at once.
inline constexpr std::int64_t agent_launch_start_timeout_seconds = 300;
inline constexpr std::string_view agent_launch_start_timeout_reason =
    "Timed out: the Orama operator did not start this run within five minutes.";

// What a person is told about a launch: the operator's phase and message once it has reported,
// otherwise why the run is still waiting, or why lake stopped it (lake#442).
struct AgentLaunchProgress {
    std::string phase, message;
};
AgentLaunchProgress agent_launch_progress(const AgentLaunch& launch,
                                          std::chrono::sys_seconds now);

// A store timestamp, YYYY-MM-DDTHH:MM:SSZ, as a time point; nothing when it is not one.
std::optional<std::chrono::sys_seconds> utc_seconds(std::string_view text);

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
