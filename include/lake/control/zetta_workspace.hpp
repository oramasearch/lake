#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/zetta_control.hpp"

namespace lake::control {

// A customer's workspace: the Zetta cluster lake's Design, Train and Ship pages delegate to
// (liquid-bridge decision 45, docs/api/workspaces.md). Zetta has no tenancy, so each customer gets
// its own cluster. lake asks the operator for one by appending a `desired` event to
// orama_control.workspaces on its data-plane Zetta; the operator creates the cluster, mounts its
// admin token into lake as `<token_dir>/<slug>`, and appends `status` events.
struct WorkspaceConfig {
    std::string lake_name;     // the Lake resource name, the `lake` column
    std::string control_url;   // LAKE_DATA_PLANE_URL
    std::string control_token; // LAKE_DATA_PLANE_TOKEN
    std::string token_dir;     // LAKE_WORKSPACE_DIR
    std::string url_template;  // LAKE_WORKSPACE_URL_TEMPLATE, with `{workspace}` for the slug

    bool configured() const {
        return !lake_name.empty() && !control_url.empty() && !control_token.empty() &&
               !token_dir.empty() && url_template.find("{workspace}") != std::string::npos;
    }
};

// `ws-` followed by the first 12 hexadecimal digits of the customer id; empty when the id is not
// a UUID. Deterministic, so lake stores nothing to find a customer's workspace.
std::string workspace_slug(std::string_view customer_id);

struct WorkspaceState {
    std::string slug;
    bool requested = false; // a desired event exists
    std::string phase;      // latest operator status: Provisioning, Ready, Failed; empty if none
    std::string message;
    bool credential = false; // the operator's mounted token is present

    // lake calls the cluster only when the operator has reported it Ready and delivered its token.
    bool ready() const {
        return credential && phase == "Ready";
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
                     std::shared_ptr<ZettaControl> control,
                     std::shared_ptr<WorkspaceCaller> caller);

    const WorkspaceConfig& config() const {
        return config_;
    }

    liquid::runtime::task<ZettaControlResult<WorkspaceState>> state(std::string customer_id);
    // Appends a desired event. Asking again for an existing workspace is harmless: the operator
    // keys clusters by slug.
    liquid::runtime::task<ZettaControlResult<WorkspaceState>> request(std::string customer_id);
    // Calls `capability` on the customer's own cluster. Refuses when the workspace is not ready.
    liquid::runtime::task<ZettaControlResult<nlohmann::json>>
    call(std::string customer_id, std::string capability, bool read, nlohmann::json input);

private:
    std::string token_for(std::string_view slug) const;

    WorkspaceConfig config_;
    std::shared_ptr<ZettaControl> control_;
    std::shared_ptr<WorkspaceCaller> caller_;
};

} // namespace lake::control
