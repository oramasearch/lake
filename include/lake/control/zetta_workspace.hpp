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
#include <utility>
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

// One request to the operator's POST /v1/service_connection (orama-operator#773,
// docs/control-plane/services.md#connections-on-demand), and its answer. The exchange is injected
// (WorkspaceService::set_connection_http): the server's outbound client in lake, a test double in
// the tests. `ca_bundle` is the control plane's CA certificate (PEM), the only root the exchange
// trusts. The request carries the Lake's connection credential and the answer carries a
// connection: neither is ever logged, written to disk or put in an error message.
struct ServiceConnectionRequest {
    std::string url;
    std::string body; // {"service": "<slug>"}
    std::vector<std::pair<std::string, std::string>> headers;
    std::string ca_bundle;
};
struct ServiceConnectionResponse {
    int status = 0;
    std::string body;
    std::string error; // the transport's failure, never the request
};
using ServiceConnectionHttp =
    std::function<ServiceConnectionResponse(const ServiceConnectionRequest&)>;

// How long a connection fetched from the operator is kept in memory before it is asked again, how
// long an answer that there is none is kept, and how long a kept connection may still be used
// while the operator cannot be reached (a roll of the operator, a network blip). Memory only.
inline constexpr std::chrono::seconds service_connection_ttl{30};
inline constexpr std::chrono::seconds service_connection_missing_ttl{5};
inline constexpr std::chrono::seconds service_connection_stale_limit{300};

// A customer's managed services (docs/control-plane/services.md, liquid-bridge decision 46): Zetta
// and Postgres clusters that lake records in its control Postgres and the Orama operator runs.
// lake reads each Ready service's connection in one of two ways:
//   - on demand (orama-operator#773): when LAKE_SERVICE_CONNECTION_URL is set, lake asks the
//     operator for one connection when it needs it, with its own credential
//     (LAKE_SERVICE_CONNECTION_TOKEN_FILE) over TLS to the control plane's CA
//     (LAKE_SERVICE_CONNECTION_CA_FILE), and keeps it only in memory, briefly;
//   - from the mount (older operators): `<service_dir>/<slug>.json`, the aggregate Secret
//     lake-<lake>-services mounted at LAKE_SERVICE_DIR.
// The customer's workspace, the Zetta cluster Design, Train and Ship delegate to
// (docs/api/workspaces.md), resolves its Zetta service through workspaces.zetta_service_id.
struct WorkspaceConfig {
    std::string service_dir;          // LAKE_SERVICE_DIR
    std::size_t max_per_customer = 0; // LAKE_SERVICES_PER_CUSTOMER; 0 (the default) means no limit
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

    // On-demand connections (orama-operator#773). The operator renders all three; when the URL is
    // set, the mount is not read.
    std::string service_connection_url;        // LAKE_SERVICE_CONNECTION_URL
    std::string service_connection_token_file; // LAKE_SERVICE_CONNECTION_TOKEN_FILE
    std::string service_connection_ca_file;    // LAKE_SERVICE_CONNECTION_CA_FILE
    // How long a fetched connection, and an answer that there is none, are kept in memory.
    std::chrono::seconds connection_ttl = service_connection_ttl;
    std::chrono::seconds connection_missing_ttl = service_connection_missing_ttl;

    bool connections_on_demand() const {
        return !service_connection_url.empty();
    }
    // Services are enabled when the operator gives lake either way of reading a connection.
    bool configured() const {
        return !service_dir.empty() || connections_on_demand();
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
};

class ZettaWorkspaceCaller final : public WorkspaceCaller {
public:
    liquid::runtime::task<ZettaControlResult<nlohmann::json>> call(std::string url,
                                                                   std::string token,
                                                                   std::string capability,
                                                                   bool read,
                                                                   nlohmann::json input) override;
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
    // The operator's connection for a service, or nothing before it is delivered: asked of the
    // operator when connections are on demand (kept in memory for service_connection_ttl), else
    // read from the mount. It carries the credential: only service_connection returns it, and
    // never to a page.
    std::optional<nlohmann::json> connection(const ManagedService& service) const;
    // The exchange on-demand connections go over. The server installs its outbound client unless
    // one is already set; without one, an on-demand lake has no connection.
    void set_connection_http(ServiceConnectionHttp http);
    bool has_connection_http() const;

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
    // The operator's connection for a store, read the same way as a service's (on demand, or
    // `<service_dir>/<slug>.json` beside the services' connections), or nothing before it is
    // delivered.
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
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string customer_id, std::string workspace_id, std::string capability,
         bool read, nlohmann::json input,
         std::function<void(std::chrono::nanoseconds)> observe = {},
         bool cache_definitions = false);
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

    // One on-demand connection (orama-operator#773): `value` is empty for an answer that there is
    // none.
    struct CachedConnection {
        std::optional<nlohmann::json> value;
        std::chrono::steady_clock::time_point fetched;
        std::chrono::steady_clock::time_point expires;
    };

    std::optional<nlohmann::json> fetch_connection(const std::string& slug) const;
    WorkspaceState state_of(const std::string& customer_id,
                            const std::optional<ManagedService>& service) const;
    Result<std::optional<ManagedService>> default_service(const std::string& customer_id);

    WorkspaceConfig config_;
    std::shared_ptr<Store> store_;
    std::shared_ptr<WorkspaceCaller> caller_;
    std::mutex registry_mutex_;
    std::map<std::string, RegistryEntry> registry_cache_;
    std::uint64_t registry_epoch_ = 0;
    mutable std::mutex connections_mutex_;
    ServiceConnectionHttp connection_http_;
    mutable std::map<std::string, CachedConnection> connections_;
};

} // namespace lake::control
