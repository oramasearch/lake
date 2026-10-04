#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/audit/audit.hpp"

// Where an audit event is written (decision 52, lake#317, docs/api/audit.md#which-log). Lake's
// audit splits by subject:
//
// - the **central** hash-chained log in lake's PostgreSQL keeps control-plane and identity events:
//   sign-in, sessions, tokens, users, customers, organizations, memberships, teams, invitations,
//   grants and policy objects, workspaces themselves, SSO and SCIM, recovery and MFA, workload
//   identity and certificates, the keystore, lake's own identity, transparency and TUF, and the
//   audit capabilities;
// - each **workspace's audit**, in its own Zetta, keeps the events about that workspace's data:
//   `workspace_act`, `workspace_query` and `workspace_sql`, and the repository, application, app,
//   build, secret, drive, forge, stream, platform, store, service, agent and experiment
//   capabilities that act on an object with a workspace.
namespace lake::api {

enum class AuditLog { Central, Workspace };
std::string_view to_string(AuditLog log);

// The log a capability's events belong to; nullopt for a capability no table names (the
// classification test fails for it, so every registered capability is classified).
std::optional<AuditLog> audit_log_of(std::string_view capability);

// Every name in the two tables, for the classification test.
std::span<const std::string_view> central_audit_capabilities();
std::span<const std::string_view> workspace_audit_capabilities();

// The actions whose events are about a workspace's data: the workspace capabilities and the
// events recorded outside the registry (`push_change`, `reference_update`, `deployment_phase`).
// Migration 0063 and the copy assign the central events of these actions to their workspace.
std::vector<std::string> workspace_audit_actions();

// Where one event goes. `workspace_id` and `customer_id` are set only for the workspace log.
struct AuditRoute {
    AuditLog log = AuditLog::Central;
    std::string customer_id;
    std::string workspace_id;
};

// The one decision for a capability call, from the capability and its input. A workspace
// capability's event goes to the workspace the call acted on: the `workspace_id` Registry::call
// validated (ApiContext::audit_workspace_id), else the workspace of the repository, secret mount,
// application, app, build or agent task its input names. It stays central when:
// - the capability is central;
// - the call was refused before its handler ran (`handled` false: no credential, an invalid input,
//   a workspace of another space, or no authority), or refused for want of authority (`denied`):
//   authorization decisions are control-plane evidence, and a caller cannot write into a
//   workspace it may not act in;
// - no live workspace of the call's space can be found (a refused input, an object without a
//   workspace, a deleted workspace);
// - this lake has no workspace audit (no AuditRecorder: no workspace Zettas configured).
AuditRoute audit_route(const Capability* capability,
                       std::string_view name,
                       const nlohmann::json& input,
                       const ApiContext& context,
                       std::string_view outcome,
                       bool handled);

// The workspace a capability call acted on, before any liveness check: the `workspace_id`
// Registry::call validated (ApiContext::audit_workspace_id), else, for a workspace capability, the
// workspace of the repository, secret mount, application, app, build or agent task its input
// names. Empty when the call was refused before its handler ran or for want of authority, or when
// no store is configured. Each object lookup is one indexed PostgreSQL read of a routing row.
// The request series (lake#384) counts the request there; audit_route takes it as `acted`.
std::string acted_workspace(const Capability* capability,
                            std::string_view name,
                            const nlohmann::json& input,
                            const ApiContext& context,
                            std::string_view outcome,
                            bool handled);

// audit_route with the acted-on workspace already found, so a call looks it up once.
AuditRoute audit_route(std::string_view name,
                       const std::string& acted,
                       const ApiContext& context);

// The route of an event recorded outside the registry about one workspace's data (branch
// protection decisions at push, the push itself): that workspace when it is live and this lake has
// a workspace audit, else central.
AuditRoute workspace_audit_route(const ApiContext& context, const std::string& workspace_id);

// Records `draft` where `route` says. A workspace event is queued in the AuditRecorder (no I/O on
// the request path) and answers nullopt; a central event is signed, chained and stored now and
// answers the event.
audit::Result<std::optional<audit::Event>> record_audit(const ApiContext& context,
                                                        const audit::EventDraft& draft,
                                                        const AuditRoute& route);

} // namespace lake::api
