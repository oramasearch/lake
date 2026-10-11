#pragma once

#include <optional>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"
#include <chrono>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
// Usage over time (lake#564, docs/api/metering.md#usage-over-time): usage_over_time,
// usage_limits, set_usage_limit and record_model_usage.
void register_usage_capabilities(Registry& registry);
// Queues every build result of the space's repositories that finished in or after the UTC hour
// `since_hour` ("YYYY-MM-DDTHH:00:00Z") for its repository's workspace records, as a
// metering_usage read does (docs/api/metering.md#build-records); a result already recorded is
// written once only. Answers how many were queued.
std::expected<std::size_t, std::string> queue_repository_builds(ApiContext& context,
                                                                const std::string& customer_id,
                                                                std::string_view since_hour);
// What one usage sample read (lake#564): workspaces sampled, readings and increments added, and a
// sentence for each workspace or measure it could not read.
// Transfer outside repository sync (lake#658, docs/api/metering.md#dimensions): adds the bytes a
// capability accepted into (`in`) or answered from (`out`) a workspace to that workspace's
// `transfer`, through the context's usage recorder. Only an in-memory increment; nothing without a
// recorder or a workspace.
void record_workspace_transfer(const ApiContext& context,
                               const std::string& workspace_id,
                               std::uint64_t in,
                               std::uint64_t out);
struct UsageSampleReport {
    std::size_t workspaces = 0;
    std::size_t readings = 0;
    std::size_t increments = 0;
    std::vector<std::string> unmeasured;
};
// The owner loop's usage sample (docs/api/metering.md#what-the-sampler-reads): each live
// workspace's stored bytes by kind, and `elapsed` of instance and volume time for each running
// service, store and app, added to `context.usage_recorder`; it also queues new repository build
// results for their workspace records. Never on a request path.
liquid::runtime::task<UsageSampleReport>
sample_usage(ApiContext& context,
             std::chrono::seconds elapsed,
             std::chrono::system_clock::time_point at = std::chrono::system_clock::now());
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
// The operator's volumes (migration 0069, docs/api/volumes.md): set_workspace_storage,
// request_volume_snapshot, volume_snapshots.
void register_volume_capabilities(Registry& registry);
// The reads lake's web app needs (lake#561, docs/api/web-app.md): viewer, my_permissions,
// list_packages, list_docs, get_doc, mcp_setup.
void register_console_capabilities(Registry& registry, std::string_view version);
// Speech-to-text and workspace transcripts (lake#561, docs/api/transcripts.md): transcribe,
// create_transcript, list_transcripts, get_transcript, update_transcript, delete_transcript.
void register_transcript_capabilities(Registry& registry);
// A workspace's own AI solutions beside Orama's read-only catalogue (lake#597,
// docs/api/solutions.md): list_solutions, get_solution, create_solution, update_solution,
// delete_solution, duplicate_solution.
void register_solution_capabilities(Registry& registry);
// Drives (lake#561, docs/api/drives.md): create_drive, list_drives, get_drive, rename_drive,
// delete_drive; and a workspace-files or virtual drive's folders, files and shortcuts:
// list_drive_entries, get_drive_entry, create_drive_folder_entry, create_drive_shortcut,
// move_drive_entry, delete_drive_entry, begin_drive_upload, append_drive_upload,
// finish_drive_upload, upload_drive_entries, read_drive_file, recent_drive_entries.
void register_drives_capabilities(Registry& registry);
// A finished workspace file, as GET /drive/files/download streams it. The route authorizes and
// audits the read through get_drive_entry first; this reads the record again for the object key,
// which no capability answers.
struct WorkspaceFile {
    std::string id;
    std::string name;
    std::string content_type;
    std::string sha256;
    std::string object_key;
    std::uint64_t size = 0;
};
liquid::runtime::task<control::ZettaControlResult<WorkspaceFile>>
find_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string drive_id,
                    std::string id);
// Plaintext [offset, offset + length) of a workspace file of `size` bytes.
liquid::runtime::task<control::ZettaControlResult<std::string>>
read_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string object_key,
                    std::uint64_t size,
                    std::uint64_t offset,
                    std::uint64_t length);
// The live workspace a new repository is created in (lake#315): `input.workspace_id` must name a
// workspace of `input.customer_id`'s space that is not deleted. Anything else, including a missing
// id, is invalid_input at /workspace_id; there is no default workspace.
std::expected<control::Workspace, ApiError> repository_workspace(ApiContext& context,
                                                                 const nlohmann::json& input);
// Creates a repository's control-store row through the repository store
// (SyncStore::create_repository_content), so a repository of a workspace served from its own
// store (lake#380) is created there too, and no switch of the workspace runs meanwhile.
control::Result<control::Repository> create_repository_row(ApiContext& context,
                                                           control::NewRepository in);

// A repository-store call from an asynchronous handler, run on the blocking pool so the server
// thread never waits on a store's lane or a workspace Zetta (lake#380). A failure of the pool
// itself becomes `on_pool_error`.
template <class Callable, class Fallback>
boost::capy::task<std::invoke_result_t<Callable>> blocking_call(Callable callable, Fallback on_pool_error) {
    auto ran = co_await run_blocking(std::move(callable));
    if (!ran) {
        co_return on_pool_error(ran.error().message);
    }
    co_return std::move(*ran);
}
// The repository a Hub-compatible id names: a repository id, `models/<id>`, `<space>/<name>` or
// a bare `<name>` unique across spaces (the model page and the Hub routes, lake#561).
control::Result<control::Repository> find_hub_repository(control::Store& store,
                                                         std::string_view hub_id);
// The spaces the caller may read, newest first, as list_customers answers them ({items}); the
// web app's `viewer` lists the same spaces without a second audited call (lake#561).
ApiResult readable_customers(ApiContext& context, int limit);
// AgentCreator's structured answer, a question or a proposal: its `result` (agent-creator#1), or,
// from an AgentCreator that answered before it had one, the JSON in its reply.
nlohmann::json agent_creator_answer(const nlohmann::json& turn);
// The digest a proposal is approved by: SHA-256 of its compact JSON as lake serializes it. Stored
// with each Agent Factory proposal message, and checked by approve_agent_proposal (lake#561).
std::string agent_proposal_digest(const nlohmann::json& proposal);
// Customer Zetta workspaces: status, start, and allow-listed calls (docs/api/workspaces.md).
void register_workspace_capabilities(Registry& registry);
// The operator's one-shot copy of retired control-store tables into workspace Zettas
// (docs/architecture/workspace-records.md).
void register_workspace_record_capabilities(Registry& registry);
// The operator's move of a workspace's repositories from its space's store on lake's volume into
// the workspace's own store in its workspace Zetta (lake#380, docs/api/workspace-store.md).
void register_workspace_store_capabilities(Registry& registry);
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
// The engine's words in a lake caller's terms (lake#591): a capability lake does not serve
// (`add_dataset`, a bare `approve_plan`) named as the call that does.
std::string engine_words_in_lake_terms(std::string text);
// The `enum` the engine's OpenAPI document declares for one input field of one capability.
std::optional<nlohmann::json> engine_input_enum(const nlohmann::json& openapi,
                                                std::string_view capability,
                                                std::string_view field);
// Zetta's OpenAPI document reduced to the allow-listed paths, rebased under `base` (the agent
// path of one workspace), so a client deriving its tools from it sees exactly what it may call.
nlohmann::json workspace_openapi(const nlohmann::json& document, std::string_view base);

// A project's pipeline from a description: capture it as the intent, compile it, and point it at
// `dataset` ("<database>.<table>") with the `model` step attributes (target, algo). Answers
// {graph}. Needs context.registry (set by any registry call).
//
// `methodology` ("M-3") asks the engine for that methodology's own pipeline (zetta#1061); an
// engine that cannot run it answers not_supported. Empty = the engine picks from the words.
//
// The compiler is given the dataset's source and table and the label and entity columns as
// `hints` (zetta#1246), so the steps are named from the data: `columns` carries `label_column`
// and `entity_column` when the caller knows them; the label falls back to the model's `target`.
liquid::runtime::task<ApiResult>
compile_pipeline(ApiContext& context,
                 std::string customer_id,
                 std::string project,
                 std::string intent,
                 std::string dataset,
                 nlohmann::json model = nullptr,
                 std::string methodology = {},
                 nlohmann::json columns = nlohmann::json::object());
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
