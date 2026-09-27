#pragma once
#include "lake/api/registry.hpp"

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
void register_drive_capabilities(Registry& registry);
void register_model_capabilities(Registry& registry);
void register_transparency_capabilities(Registry& registry);
void register_timestamp_capabilities(Registry& registry);
void register_replication_capabilities(Registry& registry);
void register_application_capabilities(Registry& registry);
void register_agent_capabilities(Registry& registry);
void register_platform_capabilities(Registry& registry);
// Customer Zetta workspaces: status, start, and allow-listed calls (docs/api/workspaces.md).
void register_workspace_capabilities(Registry& registry);

// The Zetta capabilities a lake caller may reach on a workspace: reads through GET, actions
// through POST (docs/api/workspaces.md). The agent path /workspaces/<customer>/zetta/v1/ and
// workspace_query / workspace_act both consult these, so the two can never disagree.
bool workspace_read_allowed(std::string_view capability);
bool workspace_action_allowed(std::string_view capability);
// Whether Zetta takes the capability with GET: every read, and `chat`, which lake authorizes as
// an action because it loads a model.
bool workspace_uses_get(std::string_view capability);
// Zetta's OpenAPI document reduced to the allow-listed paths, rebased under `base` (the agent
// path of one workspace), so a client deriving its tools from it sees exactly what it may call.
nlohmann::json workspace_openapi(const nlohmann::json& document, std::string_view base);

} // namespace lake::api
