#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
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

struct WorkspaceCallTiming {
    std::atomic<std::size_t> count{0};
    std::atomic<std::int64_t> nanoseconds{0};
    std::atomic<std::size_t> completed_rounds{0};
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
    // Why a presented credential was not accepted: "invalid" or "expired" (lake#450); empty when
    // none was presented or it was accepted.
    std::string credential_rejection;
    std::string workspace_id; // explicitly supplied by the capability or workspace URL
    std::string path_workspace_id; // immutable selection from /w/<workspace>/...
    std::string remembered_workspace_id; // UI navigation only; never a capability default
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
    WebhookSender webhook_sender;
    SyncStore* sync_store = nullptr;
    MeteringConfig metering;
    // Unique bytes the OCI registry holds below `<space>/` (images and their build cache), each
    // digest counted once (docs/api/metering.md). Injected by the HTTP server, which owns the
    // registry storage; null when no registry is configured, so usage reports images unmeasured.
    std::function<std::optional<std::uint64_t>(const std::string& space)> registry_space_bytes;
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
    // GET-page navigation resolves the same authorized customer list once per request.
    std::shared_ptr<std::optional<ApiResult>> page_customers;
    // Only renderer definition reads opt in; edits must read the current graph.
    bool cache_workspace_definitions = false;
    // Sends a turn to an agent's service (docs/api/agent-launches.md); null without agents.
    control::AgentCaller* agent_caller = nullptr;
    // Agent traffic over Zetta pub/sub (docs/api/agent-bus.md); null when not configured.
    control::AgentTaskBus* agent_bus = nullptr;
    // Ship Apps configuration supplied by the Lake operator (docs/control-plane/web-apps.md).
    std::string apps_wildcard;
    std::string apps_runtime_image;
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
    std::string request_origin;
    std::string caller_origin;
    std::string pool_id = "lake";
    std::vector<std::string> scopes;
    control::AuthorizationCache authorization_cache;
    std::string audit_customer_id;
    std::string audit_kind;
    std::string audit_object;
    nlohmann::json audit_details = nlohmann::json::object();
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
