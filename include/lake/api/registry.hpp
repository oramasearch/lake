#pragma once
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    std::string code;    // invalid_input | unauthorized | not_found | conflict | unrecoverable |
                         // unknown_capability | store_unavailable | store_error
    std::string message; // the real reason, verbatim where one exists
    std::string path;    // JSON pointer when the input is at fault
};

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
    std::string authentication_method;
    std::string device;
    std::string credential_device_id;
    std::string credential_session_id;
    std::string credential_kind;
    std::string credential_workload_id;
    std::string credential_customer_id;
    std::optional<std::int64_t> credential_expires_at_unix;
    std::string identity_seed;
    std::string instance_kek;
    WebhookSender webhook_sender;
    SyncStore* sync_store = nullptr;
    MeteringConfig metering;
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
    // Sends a turn to an agent's service (docs/api/agent-launches.md); null without agents.
    control::AgentCaller* agent_caller = nullptr;
    // Agent traffic over Zetta pub/sub (docs/api/agent-bus.md); null when not configured.
    control::AgentTaskBus* agent_bus = nullptr;
    // Ship Apps configuration supplied by the Lake operator (docs/control-plane/web-apps.md).
    std::string apps_wildcard;
    std::size_t apps_per_customer = 0;
    std::size_t app_replicas_per_customer = 0;
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
};

// Recursive boundary validation: required and declared keys, JSON types and array
// items. Errors carry an escaped JSON pointer to the failing field or item.
ApiResult validate_input(const Capability& capability, const nlohmann::json& input);

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
