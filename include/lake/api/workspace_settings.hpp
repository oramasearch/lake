#pragma once

#include <string>

#include <liquid/runtime.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::api {

// A workspace's settings (lake#1230, docs/api/workspaces.md#settings): `get_workspace_settings`
// answers them, with the served chat models and their prices from the price list in force, to
// anyone who may read the workspace's account; `set_workspace_settings` changes them, for those
// who may change it. They are a sealed `lake_workspace_settings` record in the workspace Zetta.
void register_workspace_settings_capabilities(Registry& registry);

// The model the workspace's agent turns call (docs/api/model-gateway.md#the-workspaces-model): its
// chosen `agent_model`, or default_agent_model when none was chosen or this Lake has no workspace
// Zettas. A workspace whose settings cannot be read answers that error.
liquid::runtime::task<control::ZettaControlResult<std::string>>
workspace_agent_model(const ApiContext& context, std::string customer_id, std::string workspace_id);

} // namespace lake::api
