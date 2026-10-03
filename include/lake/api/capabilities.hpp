#pragma once

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"
#include <map>
#include <string>
#include <string_view>

#include <liquid/runtime.hpp>

namespace lake::api {

// The core capability set: server_info (no auth) and the control-plane entities — users,
// organizations, customers — as list / get / create. Handlers are thin callers of the Store.
void register_core_capabilities(Registry& registry, std::string_view version);
void register_secret_capabilities(Registry& registry);
void register_offline_recovery_capabilities(Registry& registry);
void register_forge_capabilities(Registry& registry);
void register_export_capabilities(Registry& registry);
void register_github_import_capabilities(Registry& registry);
void register_pki_capabilities(Registry& registry);
void register_metering_capabilities(Registry& registry);
void register_drive_capabilities(Registry& registry);
void register_model_capabilities(Registry& registry);
void register_transparency_capabilities(Registry& registry);
void register_timestamp_capabilities(Registry& registry);
void register_replication_capabilities(Registry& registry);
void register_application_capabilities(Registry& registry);
void register_agent_capabilities(Registry& registry);
void register_platform_capabilities(Registry& registry);
void register_web_app_capabilities(Registry& registry);
void register_build_capabilities(Registry& registry);
void register_stream_capabilities(Registry& registry);
void register_experiment_capabilities(Registry& registry);
// The live workspace a new repository is created in (lake#315): `input.workspace_id` must name a
// workspace of `input.customer_id`'s space that is not deleted. Anything else, including a missing
// id, is invalid_input at /workspace_id; there is no default workspace.
std::expected<control::Workspace, ApiError> repository_workspace(ApiContext& context,
                                                                 const nlohmann::json& input);
// Customer Zetta workspaces: status, start, and allow-listed calls (docs/api/workspaces.md).
void register_workspace_capabilities(Registry& registry);
// The operator's one-shot copy of retired control-store tables into workspace Zettas
// (docs/architecture/workspace-records.md).
void register_workspace_record_capabilities(Registry& registry);
// Running and talking to agents (docs/api/agent-launches.md): launch_agent, list_agent_launches,
// stop_agent_launch, schedule_agent, list_agent_schedules, unschedule_agent, agent_turn. Registered
// by register_workspace_capabilities.
void register_agent_run_capabilities(Registry& registry);

// The Zetta capabilities a lake caller may reach on a workspace: reads through GET, actions
// through POST (docs/api/workspaces.md). The agent path /workspaces/<customer>/zetta/v1/ and
// workspace_query / workspace_act both consult these, so the two can never disagree.
bool workspace_read_allowed(std::string_view capability);
bool workspace_action_allowed(std::string_view capability);
// Whether Zetta takes the capability with GET: every read, and `chat`, which lake authorizes as
// an action because it loads a model.
bool workspace_uses_get(std::string_view capability);
// The graphs a Project Agent compile check stores on a project are named with this prefix
// (lake#438). They are lake's, not the person's: every list of a project's pipelines and the pick
// after a build skip them through these two functions.
inline constexpr std::string_view project_agent_check_prefix = "Project Agent check";
bool is_project_agent_check_graph(const nlohmann::json& graph);
nlohmann::json without_project_agent_checks(const nlohmann::json& graphs);
liquid::runtime::task<control::ZettaControlResult<nlohmann::json>>
call_workspace(ApiContext& context,
               std::string customer_id,
               std::string capability,
               bool read,
               nlohmann::json input);
// One name per concept on the workspace engine's capabilities (docs/api/naming.md): the engine
// takes `run_id` on some calls and `run` on others, `model_id` and `model`, `index_id` and
// `index`, `job_id` and `job`. Lake accepts either spelling and sends the one this capability
// takes. Answers the renames made, as {engine field: caller field}.
std::map<std::string, std::string> canonical_workspace_input(std::string_view capability,
                                                             nlohmann::json& input);
// A workspace engine refusal as a lake error (docs/api/errors.md): the code in lake's vocabulary
// and `path` set whenever the engine's message names an input field or a value the caller sent,
// below `prefix` (`/input` for workspace_query and workspace_act). `renamed` maps the engine's
// field names back to the caller's.
ApiResult workspace_failure(const control::ZettaControlError& error,
                            const nlohmann::json& input,
                            std::string_view prefix = {},
                            const std::map<std::string, std::string>& renamed = {});
// Zetta's OpenAPI document reduced to the allow-listed paths, rebased under `base` (the agent
// path of one workspace), so a client deriving its tools from it sees exactly what it may call.
nlohmann::json workspace_openapi(const nlohmann::json& document, std::string_view base);

// A project's pipeline from a description: capture it as the intent, compile it, and point it at
// `dataset` ("<database>.<table>") with the `model` step attributes (target, algo). Answers
// {graph}. Needs context.registry (set by any registry call).
//
// `methodology` ("M-3") asks the engine for that methodology's own pipeline (zetta#1061); an
// engine that cannot run it answers not_supported. Empty = the engine picks from the words.
liquid::runtime::task<ApiResult> compile_pipeline(ApiContext& context,
                                                  std::string customer_id,
                                                  std::string project,
                                                  std::string intent,
                                                  std::string dataset,
                                                  nlohmann::json model = nullptr,
                                                  std::string methodology = {});
// The workspace engine's methodologies (its list_methodologies answer, zetta#1061), or null
// when the engine predates the registry: then only M-1 (churn / GBDT classification) runs.
liquid::runtime::task<nlohmann::json> workspace_methodologies(ApiContext& context,
                                                              std::string customer_id);
// The registry entry an Opportunity or a design names: an "M-n" id written in `methodology`, else
// the methodology that serves the catalogue solution ("S-n") in `solution`. Null when neither does.
nlohmann::json find_methodology(const nlohmann::json& methodologies,
                                std::string_view methodology,
                                std::string_view solution);
// "Segmentation / clustering (M-3) does not run in this workspace's engine yet (zetta#1042)."
std::string methodology_waits(const nlohmann::json& entry);
// Points an existing pipeline's first connect step at `dataset` (removing the other connect steps)
// and sets its model step's attributes.
liquid::runtime::task<ApiResult> point_pipeline(ApiContext& context,
                                                std::string customer_id,
                                                std::string project,
                                                std::string graph,
                                                std::string dataset,
                                                nlohmann::json model = nullptr);
// A finished Project Agent build (kind "build:<project>") applied to the project, once: compiled
// when the project has no pipeline, otherwise its first unapproved pipeline pointed at the design's
// dataset with the design's target and model.
liquid::runtime::task<ApiResult> settle_build(ApiContext& context, control::AgentTask task);
// Confirm a finished Project Agent solution proposal (kind "solution:<project>") by calling the
// engine's create_solution once. `edits` is a JSON merge patch over result.foundation. The agent
// never creates the solution itself; only this person-confirmed path does.
liquid::runtime::task<ApiResult> settle_solution(ApiContext& context,
                                                 control::AgentTask task,
                                                 nlohmann::json edits = nlohmann::json::object());

} // namespace lake::api
