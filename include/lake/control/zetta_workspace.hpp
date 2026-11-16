#pragma once

#include <cstddef>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// Installs the egress allowlist zetta's client consults for every connection lake makes to a Zetta
// (the bus and each workspace Zetta). zetta v0.5.0's client refuses all but loopback until one is
// installed (lake#1097); the operator renders the list as ZETTA_EGRESS_ALLOW, in zetta's syntax
// (zetta docs/governance/egress-policy.md). Returns the error for a malformed list, which is not
// installed, or nullopt.
std::optional<std::string> install_zetta_egress_policy(std::string_view allow);

// A customer's managed services (docs/control-plane/services.md, liquid-bridge decision 46): Zetta
// and Postgres clusters that lake records in its control Postgres and the Orama operator runs.
// The operator mounts each Ready service's connection into lake as `<service_dir>/<slug>.json`.
// The customer's workspace, the Zetta cluster Design, Train and Ship delegate to
// (docs/api/workspaces.md), resolves its Zetta service through workspaces.zetta_service_id.
struct WorkspaceConfig {
    std::string service_dir;          // LAKE_SERVICE_DIR
    std::size_t max_per_customer = 0; // LAKE_SERVICES_PER_CUSTOMER; 0 (the default) means no limit
    // LAKE_WORKSPACES_PER_CUSTOMER: live workspaces per customer (lake#987); 0 (the default) means
    // no limit.
    std::size_t max_workspaces_per_customer = 0;
    // The Agent Library's running agents (LAKE_AGENT_ENDPOINTS, set by the operator), keyed by
    // repository name (agent_library.hpp).
    std::map<std::string, std::string> agent_endpoints;
    // The checkpoint a native agent built on "the Orama base model" loads inside a workspace
    // (LAKE_BASE_AGENT_MODEL; docs/api/train-agents.md): where the Orama operator copies the base
    // model onto every workspace Zetta's data volume, /data (lake#246).
    std::string base_agent_model = "/data/models/base-agent";
    // The frontier model a Python agent built from the form runs on (LAKE_AGENT_MODEL).
    std::string frontier_model = "gpt-5.5";
    // lake's in-cluster address, handed to agents so they reach it without leaving the cluster
    // (LAKE_INTERNAL_URL), and the bearer lake presents to agent services
    // (LAKE_AGENT_SERVICE_TOKEN).
    std::string internal_url;
    std::string agent_service_token;
    // Each library agent's own credential on the agent bus (docs/api/agent-bus.md): a file per
    // agent in LAKE_AGENT_BUS_DIR, named by its key, which the operator mounts into that agent
    // alone. Never the service token, which user-created agents on agent-runtime also hold.
    std::map<std::string, std::string> agent_bus_tokens;
    std::map<std::string, int> agent_lease_seconds;

    bool configured() const {
        return !service_dir.empty();
    }
};

// Reads LAKE_AGENT_BUS_DIR: one file per library agent, its name the agent's key and its
// content (trimmed) the agent's bus credential. Missing or empty files are skipped.
std::map<std::string, std::string> read_agent_bus_tokens(const std::string& directory);

// `ws-` followed by the first 12 hexadecimal digits of the customer id; empty when the id is not
// a UUID. The slug of a customer's first workspace, which decision 45 created clusters under.
std::string workspace_slug(std::string_view customer_id);

// A service as the API and the pages show it: never its credential.
nlohmann::json service_json(const ManagedService& service);
// The one status a person is told about a service, shared by list_services and the Services page
// so the two never disagree (lake#452), and, when the service cannot progress on its own, why.
// `connected` is whether lake has the service's connection in its mount.
struct ServiceCondition {
    std::string status, reason;
};
ServiceCondition service_condition(const ManagedService& service, bool connected);
// A workspace row as JSON. `storage_reason` is a short code from the operator's storage phase
// (pending | resizing | ready | failed, or empty) that any reader may see; `storage_message`, the
// operator's own text (it can carry Kubernetes API errors), is included only for the operator and
// the instance administrator (`operator_detail`).
nlohmann::json workspace_json(const Workspace& workspace, bool operator_detail = false);
// The short storage reason code of an operator storage phase.
std::string workspace_storage_reason(std::string_view phase);
// A store as the API and the pages show it (docs/control-plane/storage.md): never its credential.
nlohmann::json store_json(const ManagedStore& store);
// Where a store's contents are (lake#664): a bucket's {bucket, endpoint, region, prefix,
// credential: {mount_id, name}}, a `lake` store's {repository_id}, or null. Never a credential's
// value.
nlohmann::json store_location_json(const ManagedStore& store);

struct WorkspaceState {
    std::string slug, service_id, workspace_id;
    bool requested = false; // a default Zetta service is present
    std::string phase;      // the operator's: Provisioning, Ready, Failed, Deleting; empty if none
    std::string message;
    std::string endpoint;
    bool credential = false; // the operator's mounted connection is present

    // lake calls the cluster only when the operator has reported it Ready, with an address, and
    // delivered its token.
    bool ready() const {
        return credential && phase == "Ready" && !endpoint.empty();
    }
    nlohmann::json to_json() const;
};

// The per-call dataset scope (lake#1055, zetta#1515, docs/api/agent-launches.md#a-runs-dataset-
// work-area). Lake sends a dataset-confined credential's work area to the workspace Zetta as this
// header, so the engine, which parses the call, refuses a read outside it. The header only ever
// narrows what lake's own bearer may do there.
inline constexpr std::string_view dataset_scope_header = "Zetta-Dataset-Scope";
// The header's value for a work area (each `<database>/<name>`, as dataset_work_area answers):
// the datasets joined by ", ", or empty for a work area of no dataset.
std::string dataset_scope_header_value(const std::vector<std::string>& area);
// The engine capabilities whose reads the workspace Zetta checks against the header: `sql`, a
// SELECT of scoped tables (zetta#1515), and `list_datasets` and `get_dataset`, which answer only
// the scope's datasets (zetta#1519). Under a scope the engine refuses every other capability,
// because it does not declare what it reads; lake proves its own allowed calls of those
// (get_agent, workspace_openapi, ...) itself and sends them without the header.
bool engine_checks_dataset_scope(std::string_view capability);
// Of those, the ones lake cannot prove inside the work area on its own (`sql`): on an engine that
// does not enforce the header they are refused, never sent. The others (list_datasets,
// get_dataset) lake also confines itself (registry.cpp), so on such an engine they are sent
// without the header, as before.
bool engine_must_enforce_dataset_scope(std::string_view capability);
// Whether a workspace engine answer to a probe under a dataset scope shows that the engine
// enforces the scope: a Zetta that knows the header refuses `server_info` under any scope with
// `unauthorized` and `details.dataset_scope`, and one that does not know it answers the call.
bool dataset_scope_refusal(const ZettaControlError& error);

// One capability call on a workspace cluster. Injected so tests need no HTTP server.
class WorkspaceCaller {
public:
    virtual ~WorkspaceCaller() = default;
    // GET /v1/<capability>?<input> for a read, POST /v1/<capability> with a JSON body otherwise.
    // Returns the envelope's `value`, or Zetta's error message.
    virtual liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string url,
         std::string token,
         std::string capability,
         bool read,
         nlohmann::json input) = 0;
    // The same call confined to `dataset_scope` (dataset_scope_header_value) by the
    // Zetta-Dataset-Scope header. The default sends it without the header, as a Zetta that does
    // not know the header would treat it; WorkspaceService probes before it relies on the engine.
    virtual liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    scoped_call(std::string url,
                std::string token,
                std::string capability,
                bool read,
                nlohmann::json input,
                std::string dataset_scope) {
        static_cast<void>(dataset_scope);
        return call(std::move(url), std::move(token), std::move(capability), read,
                    std::move(input));
    }
};

class ZettaWorkspaceCaller final : public WorkspaceCaller {
public:
    liquid::runtime::task<ZettaControlResult<nlohmann::json>> call(std::string url,
                                                                   std::string token,
                                                                   std::string capability,
                                                                   bool read,
                                                                   nlohmann::json input) override;
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    scoped_call(std::string url,
                std::string token,
                std::string capability,
                bool read,
                nlohmann::json input,
                std::string dataset_scope) override;

private:
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    send(std::string url,
         std::string token,
         std::string capability,
         bool read,
         nlohmann::json input,
         std::optional<std::string> dataset_scope);
};

// Where a workspace's Zetta answers and the token lake presents to it (the operator's mounted
// connection), for the object routes lake's workspace store uses (lake#380).
struct WorkspaceEndpoint {
    std::string url;
    std::string token;
};

class WorkspaceService {
public:
    WorkspaceService(WorkspaceConfig config,
                     std::shared_ptr<Store> store,
                     std::shared_ptr<WorkspaceCaller> caller);

    const WorkspaceConfig& config() const {
        return config_;
    }

    // Services. Every call is on behalf of `customer_id`; a service of another customer is
    // not_found.
    Result<std::vector<ManagedService>> services(const std::string& customer_id);
    Result<ManagedService> service(const std::string& customer_id, const std::string& service_id);
    // Explicit workspace routing. Missing, foreign, non-Zetta and withdrawn IDs are invalid.
    Result<ManagedService> workspace(const std::string& customer_id,
                                     const std::string& workspace_id);
    Result<ManagedService> create_service(NewManagedService service);
    Result<ManagedService> delete_service(const std::string& customer_id,
                                          const std::string& service_id);
    // The operator's mounted connection for a service, or nothing before it is delivered. It
    // carries the credential: only service_connection returns it, and never to a page.
    std::optional<nlohmann::json> connection(const ManagedService& service) const;

    // Stores (docs/control-plane/storage.md). Every call is on behalf of `customer_id`; a store of
    // another customer is not_found. A non-empty `workspace_id` narrows the call to that
    // workspace's stores (lake#465): another workspace's store is not_found too.
    Result<std::vector<ManagedStore>> stores(const std::string& customer_id,
                                             const std::string& workspace_id = {});
    Result<ManagedStore> store(const std::string& customer_id, const std::string& store_id,
                               const std::string& workspace_id = {});
    // Refuses a type × provider pair that is not in the provider table or not available on this
    // lake, with invalid_input naming the reason.
    Result<ManagedStore> create_store(NewManagedStore store);
    Result<ManagedStore> delete_store(const std::string& customer_id, const std::string& store_id,
                                      const std::string& workspace_id = {});
    // The operator's connection for a store, `<service_dir>/<slug>.json` beside the services'
    // connections, or nothing before it is delivered.
    std::optional<nlohmann::json> store_connection(const ManagedStore& store) const;

    // Legacy default status, and explicit workspace status for data-plane callers.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>> state(std::string customer_id);
    liquid::runtime::task<ZettaControlResult<WorkspaceState>>
    state(std::string customer_id, std::string workspace_id);
    // Legacy idempotent request for the named `workspace` in the prod environment.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>>
    request(std::string customer_id, std::string requested_by = {});
    // Marks the default service absent: the operator deletes the cluster, its data volume and
    // its token, which cannot be undone. Refuses when no workspace is requested.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>> withdraw(std::string customer_id);
    // Calls `capability` on the customer's own cluster. Refuses when the workspace is not ready.
    // `dataset_scope`, for a call of a dataset-confined credential that the engine checks
    // (engine_checks_dataset_scope), is the work area (each `<database>/<name>`). On a workspace
    // Zetta seen to enforce the scope (dataset_scope_enforced) the call carries the
    // Zetta-Dataset-Scope header and is never answered from or kept in the definitions cache; a
    // scoped get_dataset must name its `database` (the engine refuses a bare name under a scope).
    // On one that does not, a capability lake cannot prove itself
    // (engine_must_enforce_dataset_scope) is refused `unauthorized` and never sent, and any other
    // is sent as an unconfined call.
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string customer_id, std::string workspace_id, std::string capability,
         bool read, nlohmann::json input,
         std::function<void(std::chrono::nanoseconds)> observe = {},
         bool cache_definitions = false,
         std::optional<std::vector<std::string>> dataset_scope = std::nullopt);
    // The workspace Zetta's address and credential, under the readiness rule call() applies:
    // workspace_not_ready until the operator reports it Ready with an address and its token.
    Result<WorkspaceEndpoint> endpoint(const std::string& customer_id,
                                       const std::string& workspace_id);
    // Invalidate on a workspace change notification; successful actions also do this.
    void invalidate_registry(const std::string& customer_id, const std::string& workspace_id);

private:
    struct RegistryEntry {
        nlohmann::json value;
        std::chrono::steady_clock::time_point expires;
        std::string customer_id;
        std::string workspace_id;
    };

    struct ScopeProbe {
        bool enforced = false;
        std::chrono::steady_clock::time_point expires;
    };

    WorkspaceState state_of(const std::string& customer_id,
                            const std::optional<ManagedService>& service) const;
    // Whether the workspace Zetta at `endpoint` enforces Zetta-Dataset-Scope: a probe of
    // `server_info` under a scope, remembered for a short time either way under `key` (the
    // service, its generation and its address), so an upgrade is seen soon and a rollback too.
    liquid::runtime::task<bool> dataset_scope_enforced(std::string key, std::string endpoint,
                                                       std::string token);
    Result<std::optional<ManagedService>> default_service(const std::string& customer_id);

    WorkspaceConfig config_;
    std::shared_ptr<Store> store_;
    std::shared_ptr<WorkspaceCaller> caller_;
    std::mutex registry_mutex_;
    std::map<std::string, RegistryEntry> registry_cache_;
    std::uint64_t registry_epoch_ = 0;
    std::map<std::string, ScopeProbe> scope_probes_;
};

} // namespace lake::control
