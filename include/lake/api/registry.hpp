#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store_contents.hpp"
#include "lake/control/workspace_store.hpp"
#include "lake/transparency/checkpoint.hpp"
#include "lake/transparency/timestamps.hpp"

namespace lake::control {
class Store;
}
namespace lake::search {
class Service;
}
namespace lake::control::pki {
class Service;
}
namespace lake::control::callers {
class Service;
}
namespace lake::control::agents {
class Service;
}
namespace lake::control {
class PlatformService;
class WorkspaceService;
class AgentCaller;
class AgentTaskBus;
} // namespace lake::control
namespace lake::transparency {
class Service;
class CheckpointService;
class TiledLog;
} // namespace lake::transparency

// The capability registry — lake's control surface in Zetta's shape: one
// registry rendered as HTTP and OpenAPI. JSON capabilities are also MCP tools;
// binary liquid sync capabilities use repository-scoped streaming routes.
namespace lake::api {

struct WorkspaceContent;
class McpHttp; // lake/api/mcp_client.hpp

struct ApiError {
    std::string code;    // one of error_codes() (docs/api/errors.md)
    std::string message; // a product-facing reason; database diagnostics remain server-side
    std::string path;    // JSON pointer when the input is at fault
    // `unauthenticated` only: why no credential was accepted (missing | invalid | expired) and
    // where to get a new one.
    std::string reason;
    std::string help;
};

// Every error code a capability answers, with its REST status and what it tells the caller
// (docs/api/errors.md). ApiResult::failure maps the engine's and liquid's spellings onto these and
// answers any other code as store_error, so a caller only ever sees a documented code.
struct ErrorCode {
    std::string_view code;
    int status;
    std::string_view meaning;
};
std::span<const ErrorCode> error_codes();
const ErrorCode* find_error_code(std::string_view code);

struct Capability;
// What a capability does, as MCP's tool annotations and mcp_setup say it (lake#594), one rule for
// both: it writes unless it is a read or authorized as one (`predict` scores without changing
// anything); it destroys only when it deletes, destroys, removes, revokes or drops something; it
// is idempotent when calling it again with the same input changes nothing more.
bool capability_writes(const Capability& capability);
bool capability_destroys(const Capability& capability);
bool capability_idempotent(const Capability& capability);

struct ApiResult {
    bool ok = false;
    nlohmann::json value;
    ApiError error;
    double duration_ms = 0.0;
    static ApiResult success(nlohmann::json output);
    static ApiResult failure(std::string code, std::string message, std::string path = {});
    nlohmann::json envelope() const;
};

// The wire contract belongs to the registry, alongside ApiResult::envelope().
nlohmann::json success_envelope_schema(const nlohmann::json& value_schema);
nlohmann::json error_envelope_schema();

struct WebhookDeliveryResult {
    int status = 0;
    std::string error;
};
using WebhookSender = std::function<WebhookDeliveryResult(
    const std::string& url, const nlohmann::json& headers, const std::string& payload)>;
using ReplicationRequester = std::function<std::expected<nlohmann::json, ApiError>(
    const std::string& source_url, std::uint64_t after_tree_size)>;

// Speech-to-text (docs/api/transcripts.md, lake#561). The audio is held only while it is sent:
// a transcriber never stores or logs it.
struct TranscriptionRequest {
    std::string file_name; // with an extension the provider recognises (audio_file_name)
    std::string mime;      // the audio's media type, or application/octet-stream
    std::string audio;     // the decoded bytes
    std::string language;  // ISO-639-1, or empty to let the provider detect it
};
struct Transcription {
    std::string text;
    // The audio's length: what the provider reports (whisper-1), else what lake measured from the
    // recording (audio_duration_seconds, lake#658).
    std::optional<double> seconds;
    std::string model;
    // The tokens the provider counted, when it reports usage in tokens (gpt-4o-transcribe) rather
    // than seconds. Detail for the audit only: speech is billed by the minute, never as tokens
    // (lake#658).
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
};
// Turns audio into text. The HTTP server injects one over its outbound client when
// LAKE_OPENAI_API_KEY is set; tests inject a fake. Empty: speech-to-text is not configured.
// It blocks for the length of the exchange, so a handler runs it with run_blocking.
using Transcriber =
    std::function<std::expected<Transcription, ApiError>(const TranscriptionRequest& request)>;

// The model gateway's provider (lake#248, docs/api/model-gateway.md, model_gateway.hpp).
// Receives a successful answer's body piece by piece as it arrives (lake#665); false stops the
// exchange, as when the caller has gone.
using ModelBodySink = std::function<bool(std::string_view piece)>;
struct ModelProviderRequest {
    std::string endpoint; // one of model_gateway_endpoints: chat/completions, responses, embeddings
    std::string body;     // the prepared request, as JSON text
    bool stream = false;
    // When set, a 2xx answer's body goes to it as it arrives and is not kept in the response's
    // body; a refusal's body is still kept whole, for its message.
    ModelBodySink on_body;
};
struct ModelProviderResponse {
    int status = 0;
    // JSON, or the whole server-sent event stream when the request streamed; empty when the
    // request's on_body took a 2xx answer
    std::string body;
    std::string error; // the transport's failure, never the request
};
// Sends one request to the model provider with Lake's own key. The HTTP server injects one over
// its outbound client when LAKE_OPENAI_API_KEY is set; tests inject a fake. Empty: no model
// provider is configured. It blocks for the length of the exchange, so a handler runs it with
// run_blocking.
using ModelProvider = std::function<ModelProviderResponse(const ModelProviderRequest& request)>;

enum class CapabilityKind { Read, Action };
std::string_view to_string(CapabilityKind kind);

enum class CapabilityTransport { Json, LiquidSync, Acme };

struct MeteringConfig {
    std::string currency = "USD";
    std::uint64_t storage_quota_bytes = 0;
    std::uint64_t storage_gib_hour_micros = 0;
    std::uint64_t transfer_in_gib_micros = 0;
    std::uint64_t transfer_out_gib_micros = 0;
    std::uint64_t compute_second_micros = 0;
};

class Registry;
class PaymentProviders; // lake/api/billing.hpp

// Repository-store work never runs on the server's thread (lake#380): a workspace's store is in
// its workspace Zetta, and a slow or hung Zetta, a lease wait or a switch must hold up only the
// requests for that store. run_blocking runs `callable` on lake's blocking pool and resumes the
// awaiting coroutine on its own executor.
namespace detail {
boost::capy::thread_pool& blocking_pool();
// Pool tasks not yet finished, posting their completion included.
std::atomic<int>& blocking_tasks();
// At most this many pool tasks are queued or running; past it run_blocking answers errc::busy at
// once instead of queueing (lake#534). The pool has 32 threads.
std::atomic<int>& blocking_task_limit();
// Completions are posted under this lock, and never to an execution context a server has
// closed (close_execution_context): a task that outlives its server's drain is abandoned.
std::mutex& completion_gate();
bool execution_context_closed(const void* context);

template <class Callable, class T, class Executor>
boost::capy::task<> run_on_blocking_pool(Callable callable,
                                         std::shared_ptr<liquid::runtime::detail::offload_state<T>> state,
                                         Executor executor) {
    try {
        state->output.emplace(std::invoke(callable));
    } catch (...) {
        state->output.emplace(std::unexpected(liquid::error{
            liquid::errc::internal, "a blocking handler threw", "lake::api::run_blocking"}));
    }
    {
        std::lock_guard gate(completion_gate());
        if (!execution_context_closed(&executor.context())) {
            boost::capy::run_async(executor)(liquid::runtime::detail::wake_offload(std::move(state)));
        }
    }
    blocking_tasks().fetch_sub(1);
    co_return;
}

template <class Callable, class T, class Executor> class blocking_awaiter {
public:
    blocking_awaiter(Callable callable,
                     std::shared_ptr<liquid::runtime::detail::offload_state<T>> state,
                     Executor executor)
        : callable_(std::move(callable)), state_(std::move(state)), executor_(std::move(executor)) {}
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle, const boost::capy::io_env*) {
        state_->completion.h = handle;
        blocking_tasks().fetch_add(1);
        boost::capy::run_async(blocking_pool().get_executor())(
            run_on_blocking_pool<Callable, T>(std::move(callable_), state_, executor_));
        return std::noop_coroutine();
    }
    void await_resume() const noexcept {}

private:
    Callable callable_;
    std::shared_ptr<liquid::runtime::detail::offload_state<T>> state_;
    Executor executor_;
};
} // namespace detail

template <class Callable>
liquid::runtime::task<std::invoke_result_t<Callable>> run_blocking(Callable callable) {
    using output_type = std::invoke_result_t<Callable>;
    if (detail::blocking_tasks().load() >= detail::blocking_task_limit().load()) {
        co_return std::unexpected(liquid::error{liquid::errc::busy,
                                                "the server's blocking pool is full; retry",
                                                "lake::api::run_blocking"});
    }
    auto state = std::make_shared<liquid::runtime::detail::offload_state<output_type>>();
    const auto executor = co_await boost::capy::this_coro::executor;
    co_await detail::blocking_awaiter<Callable, output_type, decltype(executor)>{std::move(callable), state,
                                                                                executor};
    if (!state->output.has_value()) {
        co_return std::unexpected(liquid::error{liquid::errc::internal,
                                                "a blocking handler completed without a result",
                                                "lake::api::run_blocking"});
    }
    co_return std::move(*state->output);
}

// Waits until no blocking-pool task is unfinished, or `deadline`; answers how many remain.
int wait_for_blocking_tasks(std::chrono::steady_clock::time_point deadline);
// After this, no pool task posts to `context`; a task still running is abandoned: the request
// that awaited it is never resumed, so its response and its audit event are lost. A server calls
// it before destroying its io_context, and forget_execution_context once it is destroyed.
void close_execution_context(const void* context);
void forget_execution_context(const void* context);
class MeteringRecorder;
class AuditRecorder;
class RequestRecorder;
class UsageRecorder;

struct WorkspaceCallTiming {
    std::atomic<std::size_t> count{0};
    std::atomic<std::int64_t> nanoseconds{0};
    std::atomic<std::size_t> completed_rounds{0};
    // Each workspace call by its Zetta capability, in completion order, so Server-Timing names
    // the slow call (docs/ui/performance.md).
    struct Call {
        std::string capability;
        std::int64_t nanoseconds = 0;
    };
    mutable std::mutex calls_mutex;
    std::vector<Call> calls;
    // The request's workspace store requests by kind (lake#527), added by the capability calls
    // that ran under an ObjectRequestScope on it.
    control::ObjectRequestTally store;

    void record(std::string capability, std::int64_t elapsed) {
        std::lock_guard lock(calls_mutex);
        calls.push_back({std::move(capability), elapsed});
    }
    std::vector<Call> recorded() const {
        std::lock_guard lock(calls_mutex);
        return calls;
    }
};

// What one HTTP request's capability calls resolved, for its request log line and the
// workspace's `lake_requests` series (lake#384, docs/serving/telemetry.md). Shared by every
// ApiContext the HTTP server makes for the request; Registry::call fills it, the server reads it
// once the response is sent.
struct RequestAttribution {
    mutable std::mutex mutex;
    int depth = 0;            // capability calls in progress (nested calls go through Registry)
    std::string capability;   // the first outermost capability call
    std::string workspace_id; // the first workspace a handled call acted on
    std::string request_id;   // the id the request's first context chose

    struct Snapshot {
        std::string capability;
        std::string workspace_id;
        std::string request_id;
    };
    Snapshot snapshot() const {
        std::lock_guard lock(mutex);
        return {capability, workspace_id, request_id};
    }
};

struct ApiContext {
    control::Store* store = nullptr; // null = no control-plane store configured
    // The registry running this call, set by Registry::call / call_async: a capability that is
    // several steps (the Agent Factory's approve) calls the others through it, as the same person.
    const Registry* registry = nullptr;
    std::string node_name;
    std::string acting_identity; // TAAP id, "admin" for operator, empty otherwise
    std::string acting_user_id;  // database user id for identity operations
    std::string request_id;
    std::string request_path;  // the page route, for web UI navigation; never used to authorize
    std::string request_query; // its query string, for the same; never used to authorize
    // Set by the HTTP server for a request: gives its request worker back, counted against the
    // long-request caps for `principal` (empty: the client's address). False when a cap is full.
    // call_async calls it before a long-poll capability parks (`wait_ms` > 0), so a parked caller
    // never holds one of the short-request workers (lake#534). Empty outside HTTP.
    std::function<bool(const std::string& principal)> leave_worker;
    // Why a presented credential was not accepted: "invalid" or "expired" (lake#450); empty when
    // none was presented or it was accepted.
    std::string credential_rejection;
    std::string workspace_id; // explicitly supplied by the capability or workspace URL
    std::string path_workspace_id; // immutable selection from /w/<workspace>/...
    std::string authentication_method;
    std::string device;
    std::string credential_device_id;
    std::string credential_session_id;
    std::string credential_kind;
    std::string credential_workload_id;
    std::string credential_customer_id;
    std::string credential_repository_id;
    std::string credential_revision;
    std::string credential_token_hash;
    std::optional<std::int64_t> credential_expires_at_unix;
    std::string identity_seed;
    std::string instance_kek;
    // The only key mint_sealed_secret seals to, when set (LAKE_SEALING_PUBLIC_KEY, lake#625).
    std::string sealing_public_key;
    WebhookSender webhook_sender;
    // Speech-to-text for transcribe and create_transcript (docs/api/transcripts.md); empty when
    // this lake has no OpenAI key.
    Transcriber transcriber;
    // The model gateway's provider (model_completion, docs/api/model-gateway.md); empty when this
    // lake has no OpenAI key.
    ModelProvider model_provider;
    // The caller's event stream (lake#665): set by the `/openai/v1/*` route, a streamed
    // model_completion hands it each piece of the provider's event stream as it arrives, instead of
    // answering the whole stream as `events`. Called on the outbound client's thread; false stops
    // the stream (the caller has gone). Empty: REST and MCP, which answer `events` whole.
    ModelBodySink model_stream;
    // The transport to workspace Zettas' object routes, for workspace files (drives with
    // `files` storage, docs/api/drives.md): the HTTP server's pooled client, or a test's fake.
    // Null: this lake keeps no workspace files.
    std::shared_ptr<control::ObjectTransport> object_transport;
    // Opens a bucket or filesystem store's contents for a drive with `storage: store` (lake#656,
    // store_contents.hpp). Empty: Lake browses no provider yet (lake#664), and such a drive is
    // refused with the reason.
    control::StoreBrowser store_browser;
    SyncStore* sync_store = nullptr;
    MeteringConfig metering;
    // Repository transfer and build records on their way to the workspace Zettas (lake#348,
    // docs/api/metering.md). Null: this lake records no repository usage (no workspaces).
    MeteringRecorder* metering_recorder = nullptr;
    // Where a workspace's content lives outside PostgreSQL (the instance registry's tenant images),
    // for the workspace deletion cascade (lake#514, workspace_deletion.hpp). Injected by the HTTP
    // server; null when it has no registry storage.
    const WorkspaceContent* workspace_content = nullptr;
    bool sync_wire_request = false;
    std::span<const std::byte> sync_request;
    std::vector<std::byte> sync_response;
    std::optional<SyncStoreError> sync_error;
    std::string offline_grant_id;
    nlohmann::json reauthorization_results = nlohmann::json::array();
    search::Service* search = nullptr;
    control::pki::Service* pki = nullptr;
    control::callers::Service* callers = nullptr;
    control::agents::Service* agents = nullptr;
    control::PlatformService* platform = nullptr;
    control::WorkspaceService* workspaces = nullptr;
    std::shared_ptr<WorkspaceCallTiming> workspace_call_timing;
    // The HTTP request this call serves (lake#384); null outside a request (owner loop, tests).
    std::shared_ptr<RequestAttribution> request_attribution;
    // Only renderer definition reads opt in; edits must read the current graph.
    bool cache_workspace_definitions = false;
    // Sends a turn to an agent's service (docs/api/agent-launches.md); null without agents.
    control::AgentCaller* agent_caller = nullptr;
    // Agent traffic over Zetta pub/sub (docs/api/agent-bus.md); null when not configured.
    control::AgentTaskBus* agent_bus = nullptr;
    // Ship Apps configuration supplied by the Lake operator (docs/control-plane/web-apps.md).
    std::string apps_wildcard;
    std::string apps_runtime_image;
    // The methodology-runtime image a workspace methodology's image is built from and whose
    // stage and collect pods move its data (LAKE_METHODOLOGY_RUNTIME_IMAGE, lake#655).
    std::string methodology_runtime_image;
    // The oramasearch/lake-mcp-runner image a stdio MCP connection's App runs
    // (LAKE_MCP_RUNNER_IMAGE, lake#659); empty: this Lake runs no stdio servers.
    std::string mcp_runner_image;
    std::string apps_reserved_names;
    std::size_t apps_per_customer = 0;
    std::size_t app_replicas_per_customer = 0;
    // Lake's in-cluster Service URL. Build targets use its authority so pushes bypass the public
    // Ingress request-body limit.
    std::string internal_url;
    // The library agent whose bus credential authenticated the call (LAKE_AGENT_BUS_DIR), or empty.
    std::string agent_service;
    // LAKE_ALLOW_NON_EXPIRING_CREDENTIALS: the instance-wide switch over every customer's opt-in
    // to keys and tokens that never expire (lake#234).
    bool non_expiring_credentials_allowed = true;
    transparency::Service* transparency = nullptr;
    transparency::CheckpointService* checkpoints = nullptr;
    transparency::TiledLog* transparency_log = nullptr;
    transparency::TransparencySubmitter transparency_submitter;
    transparency::TimestampService* timestamping = nullptr;
    transparency::TsaSender tsa_sender;
    std::string tsa_endpoint;
    std::string tsa_trust_pem;
    transparency::TimestampPolicy timestamp_policy;
    ReplicationRequester replication_requester;
    std::string replication_token;
    bool authorized = false;
    bool operator_credential = false;
    bool session_credential = false;
    std::string public_host;
    // The configured browser-facing origin (LAKE_EXTERNAL_ORIGIN); empty when only public_host is
    // set. The web app's `viewer` reports it with the clone and push origin (lake#561).
    std::string external_origin;
    // LAKE_CLERK_PUBLISHABLE_KEY as configured; `viewer` derives Clerk's frontend host from it.
    std::string clerk_publishable_key;
    // The instance registries' catalog (npm, Python, one-component OCI) as {available, oci, npm,
    // python}, read from the store root by the HTTP server (list_packages). Empty outside HTTP.
    std::function<nlohmann::json()> artifact_catalog;
    std::string request_origin;
    std::string caller_origin;
    std::string pool_id = "lake";
    std::vector<std::string> scopes;
    control::AuthorizationCache authorization_cache;
    std::string audit_customer_id;
    std::string audit_kind;
    std::string audit_object;
    nlohmann::json audit_details = nlohmann::json::object();
    // The workspace a call acted on, for its audit event (decision 52, audit_routing.hpp): the
    // input's `workspace_id` once Registry::call has validated it, or one a handler resolved.
    std::string audit_workspace_id;
    // The workspace audit's writer (docs/api/audit.md#workspace-audit). Null: this lake has no
    // workspace Zettas, and every event is recorded in the central log.
    AuditRecorder* audit_recorder = nullptr;
    // The per-workspace request and per-model prediction series (docs/serving/telemetry.md):
    // `predict` counts each call of a model here (lake#599). Null: nothing is counted.
    RequestRecorder* request_recorder = nullptr;
    // Usage over time in each workspace Zetta (lake#564, docs/api/metering.md#usage-over-time):
    // calls, speech-to-text seconds and model tokens are added here, off the request path. Null:
    // nothing is recorded.
    UsageRecorder* usage_recorder = nullptr;
    // Lake's MCP client transport (decision 60, docs/api/mcp-connections.md): the server's outbound
    // transport, or a test's. Null: a workspace's MCP connections cannot reach their servers.
    std::shared_ptr<McpHttp> mcp_http;
    // Whether an MCP connection may name a loopback or private address (tests, and local
    // development with LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1). Never on a deployed Lake.
    bool mcp_allow_private_addresses = false;
    // How invoices are paid where billing is on (docs/api/billing.md#payment-providers): `viewer`
    // and `server_info` report its status. Null: billing is off.
    std::shared_ptr<PaymentProviders> payments;
};

using CapabilityHandler =
    std::function<ApiResult(const nlohmann::json& input, ApiContext& context)>;
using AsyncCapabilityHandler = std::function<liquid::runtime::task<ApiResult>(
    const nlohmann::json& input, ApiContext& context)>;
using CapabilityObjectResolver = std::function<std::expected<std::string, ApiError>(
    const nlohmann::json& input, ApiContext& context)>;

struct Capability {
    std::string name; // snake_case: verb-first actions, noun reads
    std::string title;
    std::string description;
    nlohmann::json input_schema;  // object schema: properties{type}, required,
                                  // additionalProperties false
    nlohmann::json output_schema; // the success value's shape
    CapabilityKind kind = CapabilityKind::Read;
    // A long poll: called with `wait_ms` > 0 it parks on a bus, so it gives its HTTP request
    // worker back first (lake#534). Declared here, never inferred from a caller's input.
    bool long_poll = false;
    // The wait a long poll applies when its caller sends no `wait_ms` (tail_experiment_events
    // waits 10 s by default): it gives its worker back for that wait too.
    std::int64_t default_wait_ms = 0;
    // Read capabilities may also accept a JSON body without becoming actions.
    bool post_alias = false;
    CapabilityTransport transport = CapabilityTransport::Json;
    bool requires_auth = true;
    std::string object;
    std::string action;
    CapabilityObjectResolver resolve_object;
    // Candidate-returning capabilities apply control::authorize_batch internally.
    bool authorizes_candidates = false;
    // Anonymous callers may proceed only when the handler verifies that the
    // returned repository is public.
    bool allows_anonymous_public_read = false;
    // Operators bypass policy decisions, but capabilities can still resolve their
    // audit target before invoking the handler.
    bool resolves_operator_audit_target = false;
    CapabilityHandler handler;
    AsyncCapabilityHandler async_handler;
    // Deprecated input names still accepted, as {alias, canonical} (docs/api/naming.md). The
    // registry moves an alias onto its canonical field before validation, refuses the two when
    // they disagree, and Registry::add lists each alias in the input schema as deprecated.
    std::vector<std::pair<std::string, std::string>> input_aliases;
    // A scope that grants this capability too: a session or app credential that may call
    // workspace_query may call each named engine read, and one that may call workspace_act each
    // named engine action (lake#650). The handler still applies an agent turn's own scopes.
    std::string granted_by_scope;
};

// The input with every deprecated alias moved onto its canonical field, or invalid_input when a
// caller sent both with different values.
std::expected<nlohmann::json, ApiError> canonical_input(const Capability& capability,
                                                        const nlohmann::json& input);

// Recursive boundary validation: required and declared keys, JSON types, enum values, numeric
// bounds (minimum, maximum and their exclusive forms) and array items, so a schema REST, OpenAPI
// and MCP publish is the contract every transport enforces. Errors carry an escaped JSON pointer
// to the failing field or item.
ApiResult validate_input(const Capability& capability, const nlohmann::json& input);

// The agent a task, launch or schedule credential belongs to (its name, else its id), or empty for
// a person's own credential. Those credentials act as the person who started the agent; this names
// the agent (lake#445).
std::string acting_agent(const ApiContext& context);

// Who is asking for a capability's input schema. An agent-task credential names its task, and the
// task names its workspace, so `workspace_id` is optional to that caller (Registry::call fills it).
enum class SchemaAudience { Caller, AgentTask };
SchemaAudience schema_audience(const ApiContext& context);

// The input schema one audience is shown. REST/OpenAPI and MCP both render it from here, so a
// capability's schema is byte-identical on every transport (lake#441).
nlohmann::json audience_input_schema(const Capability& capability, SchemaAudience audience);

class Registry {
public:
    bool add(Capability capability);
    std::shared_ptr<const Capability> find(const std::string& name) const;
    std::vector<std::shared_ptr<const Capability>> list() const;
    // Authorize (requires_auth vs context.authorized), validate, run, time and
    // audit once.
    ApiResult call(const std::string& name, const nlohmann::json& input, ApiContext& context) const;
    liquid::runtime::task<ApiResult>
    call_async(const std::string& name, const nlohmann::json& input, ApiContext& context) const;

private:
    std::vector<std::shared_ptr<const Capability>> entries_;
};

} // namespace lake::api
