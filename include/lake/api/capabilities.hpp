#pragma once

#include <optional>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"
#include <chrono>
#include <expected>
#include <map>
#include <string>
#include <span>
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
// Before delete_workspace removes a workspace's records (lake#988, LSR-121,
// docs/api/billing.md#a-deleted-workspaces-usage): flushes the usage recorder, reads every usage
// dimension of the workspace from the first day no issued invoice covers (the month after the
// account's latest invoice that is not void, else the workspace's creation, at most 400 days, the
// series' retention) through today, a point a day, and writes it as the workspace's retained
// usage in one store transaction. A repeated call replaces it. It fails, and the deletion must not
// go ahead, when the workspace's usage cannot be read in full or cannot be written.
liquid::runtime::task<std::expected<void, ApiResult>>
retain_deleted_workspace_usage(ApiContext& context, const control::Workspace& workspace);
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
// A replication source's origin, `<scheme>://<host>:<port>` with the host lower-cased and the
// port explicit, or why the URL is not one. https, or http to a loopback host (tests).
std::expected<std::string, std::string> replication_origin(std::string_view url);
// LAKE_REPLICATION_PEERS: origins separated by commas or whitespace, each normalized by
// replication_origin. A peer with a path, query or user information is refused.
std::expected<std::vector<std::string>, std::string> parse_replication_peers(std::string_view list);
// Whether `url`'s origin is one of `peers` (lake#915).
bool replication_peer_allowed(std::string_view url, const std::vector<std::string>& peers);
void register_application_capabilities(Registry& registry);
void register_agent_capabilities(Registry& registry);
void register_platform_capabilities(Registry& registry);
void register_web_app_capabilities(Registry& registry);
// When a resource fails (lake#1207, docs/api/notifications.md): list_notifications,
// dismiss_notification, get_auto_fix and set_auto_fix, general over the resource kind (`app` now,
// `agent` with lake#1209), and repair_app and change_app, which start Application Agent turns of
// kind repair and change through agent_turn.
void register_repair_capabilities(Registry& registry);
void register_build_capabilities(Registry& registry);
void register_stream_capabilities(Registry& registry);
void register_experiment_capabilities(Registry& registry);
// The operator's volumes (migration 0069, docs/api/volumes.md): set_workspace_storage,
// request_volume_snapshot, volume_snapshots.
void register_volume_capabilities(Registry& registry);
// Secure-networking desired state (lake#728, docs/api/network.md): connections, egress rules,
// tunnel grants, peers, effective rules, and reads waiting on lake#730's workspace records.
void register_network_capabilities(Registry& registry);
// The tunnel client a Developer Access recipient installs (lake#745, docs/api/tunnel-clients.md):
// tunnel_client_downloads, signed short-lived links served by GET /tunnel-clients/<platform>.
void register_tunnel_client_capabilities(Registry& registry);
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
// The quickstart process (lake#1128, docs/api/quickstart.md): start_quickstart, quickstart_status,
// train_quickstart, quickstart_snippets and start_quickstart_conversation (lake#1182), which
// compose upload_dataset, create_index, expose_index, train_model, expose_model and
// send_ask_message through the registry; the state is a sealed record per quickstart in the
// workspace Zetta.
void register_quickstart_capabilities(Registry& registry);
// Browser keys (lake#1152, docs/api/browser-keys.md): create_browser_key, list_browser_keys and
// revoke_browser_key. browser_key_json is a key as they answer it, never its secret.
void register_browser_key_capabilities(Registry& registry);
nlohmann::json browser_key_json(const control::BrowserKey& key);
nlohmann::json browser_key_schema();
// A workspace's own methodologies from Python code beside Orama's read-only catalogue (lake#655,
// docs/api/methodologies.md): draft_methodology, create_methodology, get_methodology,
// update_methodology, delete_methodology, duplicate_methodology, validate_methodology,
// run_methodology, list_methodology_runs, get_methodology_run, stop_methodology_run, and the
// runner's methodology_run_input and methodology_run_output.
void register_methodology_capabilities(Registry& registry);
// Deleting what a workspace holds (lake#645, docs/api/deletion.md): delete_dataset, delete_model,
// delete_run, delete_index (the workspace's engine) and delete_ask_conversation (its records).
void register_deletion_capabilities(Registry& registry);
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
    std::string object_key; // a store drive's file: its path in the store
    std::uint64_t size = 0;
    // A store drive's file (lake#656): the drive, and the provider's version of the bytes for the
    // ETag when it has one. Empty for a workspace file.
    std::string store_drive_id = {};
    std::string etag = {};
};
liquid::runtime::task<control::ZettaControlResult<WorkspaceFile>>
find_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string drive_id,
                    std::string id);
// The same for a file of a store drive, read from the store's provider; find_workspace_file
// answers it for a store drive's id too.
liquid::runtime::task<control::ZettaControlResult<WorkspaceFile>>
find_store_file(ApiContext& context,
                std::string customer_id,
                std::string workspace_id,
                std::string drive_id,
                std::string id);
// Plaintext [offset, offset + length) of a workspace file of `size` bytes, or with
// `store_drive_id`, of the file at path `object_key` in that store drive's store.
liquid::runtime::task<control::ZettaControlResult<std::string>>
read_workspace_file(ApiContext& context,
                    std::string customer_id,
                    std::string workspace_id,
                    std::string object_key,
                    std::uint64_t size,
                    std::uint64_t offset,
                    std::uint64_t length,
                    std::string store_drive_id = {});
// Removes everything in a store Lake keeps in the workspace Zetta (provider `zetta`, lake#664),
// before delete_store withdraws it: its items' records and their sealed bytes. A store that never
// became Ready holds nothing and succeeds at once.
liquid::runtime::task<ApiResult> empty_kept_store(ApiContext& context,
                                                  const control::ManagedStore& store);
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
// Agent Creator Agent's structured answer, a question or a proposal: its `result` (agent-creator#1), or,
// from an Agent Creator Agent that answered before it had one, the JSON in its reply.
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
// The most rows the workspace Zetta route's `sql` answers (lake#1229, docs/api/workspaces.md):
// an App aggregates in SQL or pages with LIMIT and OFFSET rather than reading a whole table.
// workspace_sql called directly by people and agents has no cap.
inline constexpr std::size_t gateway_sql_row_cap = 5000;
// `statement` (one read-only query) with a row cap applied, or why it is refused. A statement
// without its own LIMIT gets `LIMIT cap+1` (before any OFFSET), so a cap+1-th row shows the answer
// was too large and nothing is cut silently; a LIMIT above the cap is refused; a LIMIT at or under
// it runs unchanged. The refusal is the message for too_many_rows.
std::expected<std::string, std::string> cap_sql_statement(std::string_view statement,
                                                          std::size_t cap);
// too_many_rows' message when a capped query answered more than `cap` rows.
std::string too_many_rows_message(std::size_t cap);
// The allow-lists themselves, in the order the source lists them (a name may be in both).
std::span<const std::string_view> workspace_read_names();
std::span<const std::string_view> workspace_action_names();
// One allow-listed engine call as workspace_query (`read`) or workspace_act runs it: `input` is
// {customer_id, workspace_id, capability, input}, the policy, agent scopes and audit are the same,
// and a refusal's path points below `/input`. The named engine capabilities call this.
liquid::runtime::task<ApiResult>
call_workspace_capability(bool read, nlohmann::json input, ApiContext& context);

// Every allow-listed engine capability as a named capability of Lake's own (lake#650,
// docs/api/engine-capabilities.md): `/v1/<name>`, its OpenAPI operation with the engine's input
// schema and an MCP tool. Registered by register_workspace_capabilities.
void register_engine_capabilities(Registry& registry);
// The Lake capability that serves an engine capability: its own name, a clearer one where the
// engine's would collide or mislead (`gate` is check_pipeline), or the Lake capability that
// already covers it (`get_study` is get_experiment).
std::string_view engine_capability_name(std::string_view engine);
// The engine capability a Lake capability registered by register_engine_capabilities calls, or
// empty for any other name.
std::string_view engine_capability_of(std::string_view name);
// The group llms.txt lists a capability registered by register_engine_capabilities under
// (Projects, Training, Models, ...), or empty.
std::string_view engine_capability_group(std::string_view name);
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
// A database's safe mode (lake#906, docs/api/safe-mode.md): the `safe_mode_confirmation` field a
// call that runs statements against a database or connection accepts, the hex a
// `safe_mode_warning` answered in `details.confirmation`.
nlohmann::json safe_mode_confirmation_schema();
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
//
// The pipeline is named `name`, or pipeline_title(intent) when it is empty, numbered " (2)",
// " (3)" past a pipeline of the project that has the name (lake#816). The description stays the
// project's captured intent.
//
// `create_project` (lake#1193) creates the project first, its description the intent, so a new
// project's pipeline is one call (and one Ask Orama confirmation); a project that already has the
// name is refused as `conflict` at `/create_project` and nothing is created. The answer then carries
// `project_created: true`. When the compile fails after the project was created, the project stays
// and the failure says so.
liquid::runtime::task<ApiResult> compile_pipeline(ApiContext& context,
                                                  std::string customer_id,
                                                  std::string project,
                                                  std::string intent,
                                                  std::string dataset,
                                                  nlohmann::json model = nullptr,
                                                  std::string methodology = {},
                                                  nlohmann::json columns = nlohmann::json::object(),
                                                  std::string name = {},
                                                  bool create_project = false);
// A short, readable pipeline title from its description (lake#816): the first clause, without a
// leading request ("Predict which", "Build a"), helper words ("will", "likely to") or what follows
// a preposition, at most four words and 48 characters (code points), first letter capitalized.
// "Predict which customers will churn (the churned column), with the reasons" is "Customers
// churn"; "Predict: customer churn" is "Customer churn". Curly quotes, the ellipsis and dashes are
// read as plain punctuation, so none lands in a title. The web app's `pipelineTitle`
// (frontend/src/pages/design/logic.ts) follows the same rules; both are tested on the cases in
// frontend/tests/pipeline-titles.json.
std::string pipeline_title(std::string_view description);
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
