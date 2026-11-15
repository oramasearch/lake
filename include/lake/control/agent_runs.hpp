#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

// Running user-created and library agents (docs/api/agent-launches.md, lake#231): the scopes a
// launch's credential carries, the cron expressions a schedule may use (the Orama operator runs
// each schedule as a CronJob), and the injected caller that sends a turn to an agent's service.
namespace lake::control {

// The lake capabilities every launched agent's credential may call: its workspace through the
// agent path (lake#235), the repository reads a definition stored in lake needs, the workspace's
// transcripts (lake#561), the App and build reads agent-runtime gives every agent, and the
// tools of its MCP connections (lake#652; lake checks the agent's `mcp:<prefix>` skill on every call). Its skills
// add their own reads (agent_launch_scopes_for) and narrow the workspace further inside
// agent-runtime.
inline const std::vector<std::string> agent_launch_scopes{"workspace_query",
                                                          "workspace_act",
                                                          "workspace_openapi",
                                                          "list_agent_library",
                                                          "repository_tree",
                                                          "repository_file",
                                                          "list_transcripts",
                                                          "get_transcript",
                                                          "list_apps",
                                                          "get_app",
                                                          "app_logs",
                                                          "receive_app_events",
                                                          "list_builds",
                                                          "get_build",
                                                          "build_logs",
                                                          "receive_build_events",
                                                          "list_mcp_connection_tools",
                                                          "read_mcp_tool",
                                                          "call_mcp_tool"};

// The engine actions a run's code calls directly, by Lake's names (agent-runtime#11, *Direct
// actions*): check_pipeline and start_run (skill train), generate_dataset (generate-data), decide
// and record_outcome (call-api); the engine's own policy governs them. A launch's or schedule's
// `workspace_act` runs these and the actions its scopes name, nothing else (LSR-009, lake#918):
// every other change is a proposal a person confirms.
inline const std::vector<std::string> agent_run_direct_actions{
    "check_pipeline", "start_run", "generate_dataset", "decide", "record_outcome"};

// A skill a person may grant an agent (agent-runtime#7), vendored from agent-runtime's skills.json
// by tools/agents/vendor-agent-skills.py into src/control/agent_skills.cpp: its name, the label
// the Agent Factory and the agent pages show, and the Lake reads it grants. A skill's changes are
// never in a key: agent-runtime only proposes them, for a person to confirm.
struct AgentSkill {
    std::string_view name;
    std::string_view label;
    std::vector<std::string_view> reads;
};
const std::vector<AgentSkill>& agent_skills();

// The scopes of a launch of an agent with these skills: agent_launch_scopes and the reads of each
// skill it has, once each. A skill lake does not know grants nothing.
std::vector<std::string> agent_launch_scopes_for(const std::vector<std::string>& skills);

// The same, from an agent record's `skills` (anything that is not an array of strings grants
// nothing beyond agent_launch_scopes), plus its dataset work area (dataset_work_area_scopes).
std::vector<std::string> agent_launch_scopes_for(const nlohmann::json& agent);

// A dataset work area on a launch's, schedule's or service's credential (LSR-181 companion,
// lake#1055). An agent whose `work_area` is `{kind: "dataset", detail: "<id>[, <id>...]"}` gets
// one scope `dataset:<database>/<name>` per dataset, stored with the row's scopes when it is made
// and never changed. `<database>/<name>` and `<database>.<name>` are the same dataset; a bare
// `<name>` is `datasets/<name>`, Lake's default database, never a table of that name elsewhere.
// A dataset work area that is empty or names anything malformed gives the one scope `dataset:`,
// which confines the run to no dataset at all. Any other work area gives no scope.
inline constexpr std::string_view dataset_work_area_scope = "dataset:";
std::vector<std::string> dataset_work_area_scopes(const nlohmann::json& agent);
// A confined agent whose definition is in a Lake repository (`definition: {source: "lake",
// repository_id, revision}`) also gets `agent_definition:<repository id>@<revision>`, so it may
// read that one revision of that repository to load itself (agent_definition_source reads it back).
inline constexpr std::string_view agent_definition_scope = "agent_definition:";
std::optional<std::pair<std::string, std::string>>
agent_definition_source(const std::vector<std::string>& scopes);
// `<database>/<name>` for a dataset id in any of those forms, or nothing when it is malformed.
std::optional<std::string> canonical_dataset(std::string_view id);
// The datasets a credential's scopes confine it to (each `<database>/<name>`), or nullopt when
// the scopes carry no dataset work area. An empty list confines it to nothing.
std::optional<std::vector<std::string>> dataset_work_area(const std::vector<std::string>& scopes);

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

// The IANA time zone names a schedule may use, sorted (src/control/time_zones.cpp, generated by
// tools/tz/generate-time-zones.py). A CronJob's spec.timeZone takes only these (lake#586).
const std::vector<std::string_view>& iana_time_zones();

// A schedule's time zone, exactly as the IANA database spells it, or why lake refuses it: an
// unknown name, with the zone it most likely meant when only the letter case differs (lake#586).
std::expected<std::string, std::string> checked_time_zone(std::string_view name);

// What a person is told about a schedule (lake#586): the operator's phase, and a message in lake's
// words. A schedule the operator could not set up reads Failed with why and what to do, never the
// operator's or Kubernetes's own text.
struct AgentScheduleProgress {
    std::string phase, message;
};
AgentScheduleProgress agent_schedule_progress(const AgentCronSchedule& schedule);

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

// The bearer lake presents to one person's service launch (agent-runtime#14): hex
// HMAC-SHA256 of "agent-launch:<launch id>" keyed by the Lake's agent service token
// (LAKE_AGENT_SERVICE_TOKEN). The Orama operator gives the launch the same value as its
// AGENT_SERVICE_TOKEN, so the person's code in that pod never holds the Lake's token, which every
// library agent accepts, and one launch's token does not open another.
std::string agent_launch_service_token(std::string_view lake_token, std::string_view launch_id);

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
