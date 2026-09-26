#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// A customer's managed services (docs/control-plane/services.md, liquid-bridge decision 46): Zetta
// and Postgres clusters that lake records in its control Postgres and the Orama operator runs.
// The operator mounts each Ready service's connection into lake as `<service_dir>/<slug>.json`.
// The customer's workspace, the Zetta cluster Design, Train and Ship delegate to
// (docs/api/workspaces.md), is its default Zetta service.
struct WorkspaceConfig {
    std::string service_dir;          // LAKE_SERVICE_DIR
    std::size_t max_per_customer = 0; // LAKE_SERVICES_PER_CUSTOMER; 0 (the default) means no limit

    bool configured() const {
        return !service_dir.empty();
    }
};

// `ws-` followed by the first 12 hexadecimal digits of the customer id; empty when the id is not
// a UUID. The slug of a customer's first workspace, which decision 45 created clusters under.
std::string workspace_slug(std::string_view customer_id);

// A service as the API and the pages show it: never its credential.
nlohmann::json service_json(const ManagedService& service);

struct WorkspaceState {
    std::string slug, service_id;
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
    Result<ManagedService> create_service(NewManagedService service);
    Result<ManagedService> delete_service(const std::string& customer_id,
                                          const std::string& service_id);
    // The operator's mounted connection for a service, or nothing before it is delivered. It
    // carries the credential: only service_connection returns it, and never to a page.
    std::optional<nlohmann::json> connection(const ManagedService& service) const;

    // The workspace: the default Zetta service.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>> state(std::string customer_id);
    // Creates the default Zetta service; asking again for an existing one is harmless.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>>
    request(std::string customer_id, std::string requested_by = {});
    // Marks the default service absent: the operator deletes the cluster, its data volume and
    // its token, which cannot be undone. Refuses when no workspace is requested.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>> withdraw(std::string customer_id);
    // Calls `capability` on the customer's own cluster. Refuses when the workspace is not ready.
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string customer_id, std::string capability, bool read, nlohmann::json input);

private:
    WorkspaceState state_of(const std::string& customer_id,
                            const std::optional<ManagedService>& service) const;
    Result<std::optional<ManagedService>> default_service(const std::string& customer_id);

    WorkspaceConfig config_;
    std::shared_ptr<Store> store_;
    std::shared_ptr<WorkspaceCaller> caller_;
};

} // namespace lake::control
