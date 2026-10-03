#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/builds.hpp"
#include "lake/control/offline_recovery.hpp"
#include "lake/control/secrets.hpp"
#include "lake/control/web_apps.hpp"

// The control-plane store: users and organizations (identity), customers (the
// tenant that owns everything). One interface, two implementations — Postgres
// (production, the schema in db/schema.sql) and in-memory (tests, local demos).
// No HTTP type, no libpq type: the Postgres implementation lives in its own
// library.
namespace lake::control {

struct StoreError {
    std::string code; // not_found | conflict | invalid_input | store_unavailable
                      // | store_error
    std::string message;
};
template <class T> using Result = std::expected<T, StoreError>;

struct User {
    std::string id, taap_id, email, display_name, created_at, updated_at;
};
struct Organization {
    std::string id, taap_id, display_name, created_at, updated_at;
};
struct Customer {
    std::string id, customer_type, user_id, organization_id, name, created_at, updated_at;
};
struct Repository {
    std::string id, customer_id, name, description, visibility, deleted_at, retention_until,
        created_at, updated_at;
    // The workspace the repository belongs to (migration 0051, lake#315): set on create and never
    // changed. Empty only for a repository the migration recorded as needs_selection.
    std::string workspace_id;
};
struct RepositoryRef {
    std::string repository_id, name, head, updated_at;
};
struct RepositoryDeletion {
    std::string deleted_at, retention_until;
};
// Forge records retain a versioned value because review anchors must be
// replaceable by libliquid's map_anchor result without a schema migration. They live in the
// workspace Zetta of their repository's workspace (lake#341, control/forge_records.hpp); the
// control store keeps only the retired forge_records table for the one-shot copy.
struct ForgeRecord {
    std::string id, repository_id, kind, created_at, updated_at;
    nlohmann::json value;
    std::int64_t at_us = 0;         // created_at in microseconds since the Unix epoch
    std::int64_t revision = 0;      // the workspace engine's revision (conditional writes)
    std::int64_t lake_revision = 1; // lake's revision inside the seal
};
// A row of the retired forge_records table with its recorded owner (migration 0054): its
// repository's workspace, or empty with the reason it is not copied (`needs_selection` while the
// repository has no workspace, `workspace_deleted` when that workspace is gone).
// A central audit event about a workspace's data, recorded before decision 52, with the
// workspace its copy goes to (lake#317, migration 0063). An empty workspace_id stays central.
struct LegacyAuditEvent {
    audit::Event event;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome; // event_workspace | workspace_deleted
};
struct LegacyForgeRecord {
    ForgeRecord record;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
// A repository's runtime environment (docs/api/platform.md). It lives in the workspace Zetta of the
// repository's workspace (lake#358, control/platform_records.hpp); its id is
// platform_environment_id(repository, name) and names its `environment` policy object, which stays
// in the control store with its grants.
struct PlatformEnvironment {
    std::string id;
    std::string repository_id;
    std::string name;
    std::string object_id;
    std::string created_at;
    std::int64_t at_us = 0; // created_at in microseconds since the Unix epoch
};
// The last operator-control row lake observed for one deployment, environment value or domain
// (`kind` is deployment, environment_env or domain). It lives in the workspace Zetta of the
// repository the row names (lake#359, control/platform_records.hpp).
struct PlatformMirrorRow {
    std::string kind;
    std::string key;
    nlohmann::json value;
    std::string seen_at;
    std::int64_t revision = 0;      // the workspace engine's revision (conditional writes)
    std::int64_t lake_revision = 1; // lake's revision inside the seal
};
// A row of the retired platform_environments table (migration 0060) with its recorded owner: its
// repository's workspace, or empty with the reason it is not copied (`needs_selection` while the
// repository has no workspace, `workspace_deleted` when that workspace is gone). `id` is the row's
// own id, which workspace_assignments names; `environment.id` is the same legacy id, and the copy
// writes the record under the derived one.
struct LegacyPlatformEnvironment {
    std::string id;
    PlatformEnvironment environment;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
// A row of the retired platform_mirror table (migration 0060). `id` is md5(kind, newline, key) as
// a UUID (platform_mirror_id), which SQL computes the same way. `outcome` is also
// `unknown_repository` for a row naming no repository lake has.
struct LegacyPlatformMirrorRow {
    std::string id;
    PlatformMirrorRow row;
    std::int64_t at_us = 0; // seen_at in microseconds since the Unix epoch
    std::string repository_id;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
// A drive share link (docs/api/sharing.md). It lives in the workspace Zetta of the workspace that
// holds it (lake#350, control/drive_share_links.hpp); the control store keeps only its synthetic
// `share-link:<id>` grant, the content-free route of a link made before the move, and the retired
// drive_share_links table for the one-shot copy.
struct DriveShareLink {
    std::string id, repository_id, object_id, node_id, path, role, token_hash, expires_at,
        revoked_at, created_by_taap_id, created_at;
    std::int64_t at_us = 0;         // created_at in microseconds since the Unix epoch
    std::int64_t revision = 0;      // the workspace engine's revision (conditional writes)
    std::int64_t lake_revision = 1; // lake's revision inside the seal
};
// A row of the retired drive_share_links table with its recorded owner (migration 0048): the
// workspace it is copied into, or empty with the reason it is not (`needs_selection`).
struct LegacyDriveShareLink {
    DriveShareLink link;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
// Where a link made before lake#350 lives: content-free dispatch from its token digest to its
// workspace and record key (drive_share_link_routes).
struct DriveShareLinkRoute {
    std::string link_id;
    std::string workspace_id;
};
struct Team {
    std::string id, organization_id, name, display_name, created_at, updated_at;
};
struct Membership {
    std::string id, organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, created_at, updated_at;
};
struct Invitation {
    std::string id, organization_id, team_id, invitee_type, invitee, role, expires_at, status,
        accepted_membership_id, created_by_taap_id, created_at, updated_at;
};
// The result of create_organization_space and convert_personal_space. personal is the fresh
// personal space a conversion provisions; it is empty after create_organization_space.
struct OrganizationSpace {
    Organization organization;
    Customer customer;
    Membership owner;
    Customer personal;
};
struct PolicyObject {
    std::string id, object_type, external_id, parent_id, created_at, updated_at;
    bool inherit = true;
};
struct Grant {
    std::string id, principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        created_at, updated_at;
};
struct ObjectPathEntry {
    std::string requested_object_id;
    PolicyObject object;
    int depth = 0;
};
struct NewUser {
    std::string taap_id, email, display_name;
};
struct NewOrganization {
    std::string taap_id, display_name, customer_id;
};
struct NewCustomer {
    std::string customer_type, user_id, organization_id, name;
};
struct NewRepository {
    std::string customer_id, name, description,
        visibility; // visibility empty = private
    // Required: a live workspace of customer_id's space (migration 0051, lake#315).
    std::string workspace_id;
};
struct NewTeam {
    std::string organization_id, name, display_name, acting_principal, request_id;
};
struct NewMembership {
    std::string organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, request_id;
};
struct NewInvitation {
    std::string organization_id, team_id, invitee_type, invitee, role, expires_at,
        created_by_taap_id, request_id;
};
struct AcceptInvitation {
    std::string invitation_id, principal_type, principal_id, principal_taap_id, attestation_digest,
        accepted_by_taap_id, request_id;
};
// A person starts an organization: the organization, its own space (an organization customer
// with a handle, generated when name is empty), the creator's owner membership and the
// creator's admin grant on the new space, in one transaction (docs/api/organizations.md).
struct NewOrganizationSpace {
    std::string display_name, name, owner_user_id, owner_taap_id, request_id;
};
// A person turns their personal space into an organization's space. The customer keeps its id,
// handle, repositories and workspaces; the person becomes its owner and receives a fresh
// personal space, in one transaction.
struct ConvertPersonalSpace {
    std::string customer_id, display_name, owner_user_id, owner_taap_id, request_id;
};
struct NewPolicyObject {
    std::string object_type, external_id, parent_id, acting_principal, request_id;
    bool inherit = true;
};
struct NewGrant {
    std::string principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        request_id;
};

// Identity-core records carry pool_id so lake and application users share the
// same credential, device, and session tables without sharing authentication state.
struct Device {
    std::string id, user_id, name, revoked_at, created_at, updated_at;
    std::string passkey_credential_id;
    nlohmann::json passkey_binding_attestation;
};
struct Session {
    std::string id, user_id, device_id, pool_id, kind, name, reason, expires_at, revoked_at,
        created_at, last_used_at;
    std::vector<std::string> scopes;
    std::string parent_session_id;
};
struct IssuedSession {
    Session session;
    std::string secret;
    // True when this call created the user (a sign-up, or a first OAuth sign-in in a pool).
    bool created_user = false;
};
struct Principal {
    std::string user_id, taap_id, session_id, device_id, pool_id, kind;
    std::vector<std::string> scopes;
    std::optional<std::int64_t> expires_at_unix;
    std::string workload_id;
    std::string workload_customer_id;
    std::string workload_repository_id;
    std::string workload_revision;
    std::string workload_token_hash;
};
struct NewPasswordUser {
    std::string pool_id, email, password, display_name, device_name, identity_document;
    // The new user's id, chosen by the caller so that an application's workspace can record the
    // user before lake's own row exists (lake#353); empty: the store chooses one.
    std::string user_id;
};
struct PasswordSignIn {
    std::string pool_id, email, password, totp_code, device_name;
};
struct TaapSignIn {
    std::string pool_id, taap_id, assertion_id, device_name;
};
struct MfaEnrollment {
    std::string secret;
    std::vector<std::string> recovery_codes;
};
struct NewToken {
    std::string user_id, pool_id, name, reason;
    std::vector<std::string> scopes;
    int expires_in_seconds = 0;
    bool is_static = false;
    std::string parent_session_id;
};
struct WebAuthnChallenge {
    std::string user_id, pool_id, ceremony, challenge, rp_id, origin;
};
struct PasskeyRegistration {
    std::string user_id, pool_id, challenge, client_data_json, attestation_object, name;
    audit::IdentityOptions identity_options;
};
struct PasskeyAssertion {
    std::string pool_id, challenge, credential_id, client_data_json, authenticator_data, signature,
        device_name;
};
struct OAuthIdentity {
    std::string pool_id, provider, subject, email, display_name, device_name, identity_document;
};
// A person who signed in through Clerk (docs/api/clerk.md). The custodial fields carry a TAAP
// identity lake minted for them; the store keeps it only when it creates the user.
struct ClerkIdentity {
    std::string subject, email, display_name, device_name;
    std::string custodial_taap_id, custodial_public_key, custodial_wrapped_seed;
};
// A service a customer runs on the Lake's cluster (docs/control-plane/services.md): lake writes
// the desired fields, the operator the status fields.
struct ManagedService {
    std::string id, customer_id, kind, name, slug;
    bool is_default = false;
    int instances = 1, storage_gi = 10;
    std::string desired; // present | absent
    std::int64_t generation = 1;
    std::string created_by_taap_id, created_at, updated_at;
    std::string phase, message, endpoint; // empty until the operator reports
    std::int64_t observed_generation = 0;
    std::string workspace_id;
};
struct Workspace {
    std::string id, customer_id, name, environment;
    std::string desired = "running";
    std::int64_t generation = 1;
    std::string phase;
    std::int64_t observed_generation = 0;
    std::string message, namespace_name, zetta_service_id, created_at, updated_at;
};
struct NewWorkspace {
    std::string customer_id, name, environment, created_by_taap_id;
};
struct WorkspacePatch {
    std::optional<std::string> name, environment, desired;
};
// Reserve room for `-ws-` and twelve UUID digits in a DNS label.
inline bool valid_workspace_environment(std::string_view value) {
    if (value.empty() || value.size() > 47 || value.front() == '-' || value.back() == '-') {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}
// `<prefix>-` and the first 12 hexadecimal digits of a UUID, the Kubernetes suffix of a service:
// `svc-` for most, `ws-` plus the customer id for a customer's first default workspace. Empty when
// `uuid` is not one.
inline std::string managed_service_slug(std::string_view prefix, std::string_view uuid) {
    std::string hex;
    for (const char character : uuid) {
        if (character == '-') {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
            return {};
        }
        hex += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return hex.size() == 32 ? std::string(prefix) + "-" + hex.substr(0, 12) : std::string();
}
// The service catalog (docs/control-plane/services.md, liquid-bridge decision 47), in menu
// order. The schema's service_kind_check lists the same keys.
// `ready_seconds` is how long a new service of the kind usually takes from Create to Ready on
// lake's Services page, measured with tools/e2e/start-times.py and rounded up to 5 s
// (docs/api/services.md, lake#197).
// The order is the menu's: Zetta first, as the default, then the rest by label.
struct ServiceKind {
    std::string_view key, label;
    int ready_seconds = 60;
};
inline constexpr ServiceKind service_kinds[] = {
    {"zetta", "Zetta", 30},          // Steve, 2026-09-26 (measured once at 41 s)
    {"cassandra", "Cassandra", 420}, // not yet measured without a node scale-up (lake#197)
    {"clickhouse", "ClickHouse", 75},
    {"duckdb", "DuckDB", 30},
    {"kafka", "Kafka", 95},
    {"polars", "Polars", 35},
    {"postgres", "Postgres", 45},
    {"sqlite", "SQLite", 25},
    {"tidb", "TiDB", 105},
    {"valkey", "Valkey", 45},
};
inline const ServiceKind* find_service_kind(std::string_view key) {
    for (const ServiceKind& kind : service_kinds) {
        if (kind.key == key) {
            return &kind;
        }
    }
    return nullptr;
}
struct NewManagedService {
    std::string customer_id, kind, name, created_by_taap_id;
    bool is_default = false;
    int instances = 1, storage_gi = 10;
    std::size_t max_per_customer = 0; // live services per customer; 0 (the default) means no limit
    std::string workspace_id;
};
// A customer's store (docs/control-plane/storage.md, lake#196): a bucket, filesystem, network
// filesystem or volume. Two axes: the type (what a client gets) and the provider (what runs it).
// lake writes the desired fields, the operator the status fields, as for services.
struct ManagedStore {
    std::string id, customer_id, type, provider, name, slug;
    int size_gi = 10;
    std::string region;
    bool is_public = false; // private by default
    std::string desired;    // present | absent
    std::int64_t generation = 1;
    std::string created_by_taap_id, created_at, updated_at;
    std::string phase, message, endpoint; // empty until the operator reports
    std::int64_t observed_generation = 0;
    // The workspace the store belongs to (migration 0044, lake#465); the operator places it in
    // that workspace's namespace, and lake lists a workspace's own stores only.
    std::string workspace_id;
};
// A launch credential is `lal.<launch id>.<secret>`, a schedule's `las.<schedule id>.<secret>`:
// the Orama operator generates it for the pod and writes its SHA-256 into the status row it owns
// (agent_launch_status, agent_cron_schedule_status); lake compares digests and never stores it.
inline constexpr std::string_view agent_launch_credential_prefix = "lal.";
inline constexpr std::string_view agent_schedule_credential_prefix = "las.";
// A background agent turn's credential (lake#269), resolved the same way.
inline constexpr std::string_view agent_task_credential_prefix = "lat.";
inline std::optional<std::pair<std::string, std::string>>
parse_agent_credential(std::string_view secret, std::string_view prefix) {
    if (!secret.starts_with(prefix)) {
        return std::nullopt;
    }
    secret.remove_prefix(prefix.size());
    const std::size_t dot = secret.find('.');
    if (dot != 36 || secret.size() < dot + 33) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::string(secret.substr(0, dot)),
                                               std::string(secret.substr(dot + 1))};
}
inline std::optional<std::pair<std::string, std::string>>
parse_agent_launch_credential(std::string_view secret) {
    return parse_agent_credential(secret, agent_launch_credential_prefix);
}
inline bool constant_time_hex_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size() || left.empty()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

// A run of a user-created Python agent on agent-runtime (docs/api/agent-launches.md, lake#231):
// lake writes the desired launch, the Orama operator runs it (a Job for `job`, a Deployment for
// `service`) and writes the status. The pod authenticates with a credential the operator
// generates; lake keeps only its SHA-256 (`credential_sha256`, in the status the operator writes).
struct AgentLaunch {
    std::string id, customer_id, agent_id, agent_name, mode, slug, desired, trigger;
    nlohmann::json input = nullptr;
    std::vector<std::string> scopes;
    std::int64_t generation = 1;
    std::string created_by_user_id, created_by_taap_id, created_at, updated_at;
    std::string phase, message, endpoint; // empty until the operator reports
    std::optional<int> exit_code;
    nlohmann::json result = nullptr;
    std::string credential_sha256;
    std::int64_t observed_generation = 0;
    // The workspace whose Zetta holds the agent (migration 0043); the operator runs the launch in
    // that workspace's namespace and lists only that workspace's runs.
    std::string workspace_id;
    // Why lake withdrew the launch itself, e.g. the operator never started it (lake#442).
    std::string stop_reason;
};
struct NewAgentLaunch {
    std::string customer_id, agent_id, agent_name, mode, trigger;
    nlohmann::json input = nullptr;
    std::vector<std::string> scopes;
    std::string created_by_user_id, created_by_taap_id;
    std::string workspace_id;
};
// One cron schedule of a Python agent (docs/api/agent-launches.md): an agent may have several.
// lake writes the desired schedule (agent_cron_schedules, migration 0031); the Orama operator makes
// each present one a Kubernetes CronJob and writes agent_cron_schedule_status, and one
// agent_cron_runs row per Job the CronJob starts. Every run acts as whoever set the schedule.
struct AgentCronSchedule {
    std::string id, customer_id, agent_id, agent_name, cron, time_zone, description, slug, desired;
    nlohmann::json input = nullptr;
    std::vector<std::string> scopes;
    std::int64_t generation = 1;
    std::string created_by_user_id, created_by_taap_id, created_at, updated_at;
    std::string phase, message, last_schedule_at; // empty until the operator reports
    std::string credential_sha256;
    std::int64_t observed_generation = 0;
    std::string workspace_id; // migration 0043, as for launches
};
struct NewAgentCronSchedule {
    std::string customer_id, agent_id, agent_name, cron, time_zone, description;
    nlohmann::json input = nullptr;
    std::vector<std::string> scopes;
    std::string created_by_user_id, created_by_taap_id;
    std::string workspace_id;
};
// One agent turn run in the background (lake#269): lake writes it, publishes it on the agent bus
// (docs/api/agent-bus.md) and answers the request at once. `phase` is empty until an agent claims
// it, then Running. Expired leases return to waiting; the third expiry finishes it as Failed.
// A background agent turn (lake#269, docs/api/agent-bus.md). The control store keeps only its
// dispatch half: ids, space, workspace, agent, kind, the asking task, scopes, phase, claim, lease
// and attempts. What a person or an agent wrote for it (the request `body`, the agent's `message`
// and its `result`) lives in the task's workspace Zetta (lake#374, lake#375,
// control/agent_task_records.hpp), and the API layer fills those fields in from there. A task the
// control store answers has an empty `body`, a null `result`, and as `message` only lake's own
// notice (a retry, or a task nobody picked up), which outranks the agent's message.
struct AgentTask {
    std::string id, customer_id, agent, kind, conversation_id;
    std::string workspace_id;
    // The task whose agent asked for this one (lake#451); empty for a person's turn.
    std::string parent_task_id;
    nlohmann::json body = nlohmann::json::object();
    std::vector<std::string> scopes;
    int deadline_seconds = 300;
    std::string desired = "present";
    std::string requested_by_user_id, requested_by_taap_id, created_at;
    std::string phase, message, started_at, finished_at, credential_sha256;
    nlohmann::json result = nullptr;
    // The agent-events sequence at or before the task's first event (0: unknown), lake#275.
    std::uint64_t events_from = 0;
    int lease_seconds = 600;
    int attempts = 1;
    std::string lease_until;
    bool retry_notice_pending = false;
};
// A task of the retired content columns (agent_tasks.body, agent_task_status.message and
// result), with its recorded owner (migration 0062): `task` carries its body, message, result and
// phase as an earlier lake wrote them, and `at_us` its creation instant. `workspace_id` is the
// live workspace it is copied into, or empty with the reason it is not: `needs_selection` (the
// task names no workspace) or `workspace_deleted` (its record went with that workspace).
struct LegacyAgentTask {
    AgentTask task;
    std::int64_t at_us = 0;
    std::string workspace_id;
    std::string outcome;
};
struct AgentTaskStreamScope {
    std::string id, agent, finished_at;
    std::string workspace_id;
};
// What lake's bus loop acts on: unclaimed tasks to republish, waiting tasks past their deadline,
// and reclaimed tasks whose retry or final failure notice still needs publication.
struct StaleAgentTasks {
    std::vector<AgentTask> unclaimed, overdue, reclaimed;
};

// Who created a workspace topic. It lives in the workspace Zetta of that topic (lake#379,
// control/stream_owners.hpp); the control store only reads the retired stream_owners table for
// the one-shot copy.
struct StreamOwner {
    std::string customer_id, workspace_id, topic, created_by;
};
// A row of the retired stream_owners table with its recorded owner (migration 0047). `id` is the
// row's stable identity, md5(customer_id, workspace_id, topic) as a UUID, which pages the copy.
// `workspace_id` is the live workspace it is copied into, or empty with the reason it is not:
// `workspace_deleted` (the topic went with that workspace incarnation) or `unknown_workspace`.
struct LegacyStreamOwner {
    std::string id;
    StreamOwner owner;
    std::string workspace_id;
    std::string outcome;
};

// One Opportunity Auto-Suggest recommended (or declined, status not_yet), kept across scans. It
// lives in the workspace Zetta of the workspace its scan ran in (lake#376,
// control/opportunities.hpp); the control store only reads the retired opportunities table for
// the one-shot copy.
struct Opportunity {
    std::string id, customer_id, task_id;
    int rank = 0;
    std::string intent, solution, methodology, why;
    nlohmann::json payload = nlohmann::json::object();
    std::string status = "new", project, created_at;
    std::int64_t at_us = 0;        // created_at in microseconds since the Unix epoch
    std::int64_t lake_revision = 1; // 1 as the scan wrote it, one more for each status change
};

// One message of an Ask Orama conversation (docs/api/ask-orama.md, lake#236). It lives in the
// workspace Zetta of the workspace it was written in (lake#370, control/ask_messages.hpp); the
// control store holds none (migration 0050 dropped the retired ask_messages table, lake#483).
struct AskMessage {
    std::string id, conversation_id, customer_id, user_id, role, content;
    nlohmann::json payload = nullptr; // proposals, reads and delegations of an assistant message
    std::string created_at;
    std::int64_t at_us = 0; // created_at in microseconds since the Unix epoch
};
// A row of the retired opportunities table with its recorded owner (migration 0046): the
// workspace it is copied into, or empty with the reason it is not (`needs_selection`,
// `deleted_workspace`).
struct LegacyOpportunity {
    Opportunity opportunity;
    std::string workspace_id;
    std::string outcome;
};
struct NewManagedStore {
    std::string customer_id, type, provider, name, created_by_taap_id;
    int size_gi = 10;
    std::string region;
    bool is_public = false;
    std::string workspace_id; // required by the routing trigger (migration 0044)
};
// One valid type × provider pair and whether this lake offers it. Order: what is available first,
// then by type. The schema's store_type_check and store_provider_check list the same keys.
struct StoreProvider {
    std::string_view type, provider, label;
    bool available = false;
    std::string_view unavailable_reason; // empty when available
};
inline constexpr std::string_view store_phase_two_reason =
    "needs an Orama account or your own credentials (lake#196 phase 2)";
inline constexpr StoreProvider store_providers[] = {
    {"volume", "cluster", "Volume — cluster disk", true, ""},
    {"object",
     "r2",
     "Bucket — Cloudflare R2",
     false,
     "needs R2 enabled on Orama's Cloudflare account and a token that can issue bucket keys"},
    {"object", "s3", "Bucket — AWS S3", false, store_phase_two_reason},
    {"object", "b2", "Bucket — Backblaze B2", false, store_phase_two_reason},
    {"object", "azure", "Bucket — Azure Blob", false, store_phase_two_reason},
    {"object", "gcs", "Bucket — Google Cloud Storage", false, store_phase_two_reason},
    {"object", "lake", "Bucket — liquid repository", false, "phase 3"},
    {"object", "zetta", "Bucket — Zetta large objects", false, "phase 3"},
    {"filesystem", "lake", "Filesystem — Drive", false, "phase 3"},
    {"filesystem", "zetta", "Filesystem — Zetta", false, "phase 3"},
    {"network_filesystem", "nfs", "Network filesystem — NFS in the cluster", false, "phase 4"},
    {"network_filesystem", "azure", "Network filesystem — Azure Files", false, "phase 4"},
    {"network_filesystem", "s3", "Network filesystem — EFS", false, "phase 4"},
    {"network_filesystem", "gcs", "Network filesystem — Filestore", false, "phase 4"},
};
inline const StoreProvider* find_store_provider(std::string_view type, std::string_view provider) {
    for (const StoreProvider& entry : store_providers) {
        if (entry.type == type && entry.provider == provider) {
            return &entry;
        }
    }
    return nullptr;
}
struct OAuthProvider {
    std::string pool_id, provider, issuer, client_id, client_secret;
    std::vector<std::string> scopes;
};
struct OAuthFlow {
    std::string pool_id, provider, state, code_verifier, nonce, device_name, identity_document;
};
struct SsoProvider {
    std::string id;
    std::string organization_id;
    std::string pool_id;
    std::string name;
    std::string protocol;
    std::string issuer;
    std::string metadata;
    std::string signing_certificate;
    std::string client_id;
    nlohmann::json attribute_mapping;
    bool jit_provisioning = false;
    bool enabled = true;
    std::string tested_at;
    std::string created_at;
    std::string updated_at;
};
struct NewSsoProvider {
    std::string organization_id;
    std::string pool_id;
    std::string name;
    std::string protocol;
    std::string issuer;
    std::string metadata;
    std::string signing_certificate;
    std::string client_id;
    std::string client_secret;
    std::string scim_token;
    nlohmann::json attribute_mapping;
    bool jit_provisioning = false;
};
struct FederationIssuer {
    std::string id;
    std::string organization_id;
    std::string issuer;
    std::string claim;
    std::string created_at;
};
struct NewFederationIssuer {
    std::string organization_id;
    std::string issuer;
    std::string claim;
};
struct SsoIdentity {
    std::string provider_id;
    std::string issuer;
    std::string subject;
    std::string email;
    std::string display_name;
    std::string taap_id;
    std::string device_name;
    nlohmann::json claims;
};
struct ScimUser {
    std::string provider_id;
    std::string external_id;
    std::string user_id;
    bool active = true;
    nlohmann::json attributes;
    std::string created_at;
    std::string updated_at;
};
struct NewScimUser {
    std::string provider_id;
    std::string external_id;
    std::string email;
    std::string display_name;
    std::string taap_id;
    bool active = true;
    nlohmann::json attributes;
};
struct ScimGroup {
    std::string provider_id;
    std::string external_id;
    std::string team_id;
    std::string display_name;
    nlohmann::json attributes;
    std::string created_at;
    std::string updated_at;
};
struct NewScimGroup {
    std::string provider_id;
    std::string external_id;
    std::string team_name;
    std::string display_name;
    std::vector<std::string> member_external_ids;
    nlohmann::json attributes;
};
struct ProvisioningLog {
    std::string id;
    std::string provider_id;
    std::string resource;
    std::string operation;
    std::string external_id;
    std::string outcome;
    nlohmann::json details;
    std::string created_at;
};

// Private certificate-authority material is ciphertext at the store boundary.
// The concrete store envelopes each key using the owning customer's KEK.
struct PkiAuthorityRecord {
    std::string id;
    std::string customer_id;
    std::string root_certificate_pem;
    std::string intermediate_chain_pem;
    std::string root_key_ciphertext;
    std::string intermediate_key_ciphertext;
    std::string created_at;
    std::string updated_at;
};

struct NewPkiAuthorityRecord {
    std::string customer_id;
    std::string root_certificate_pem;
    std::string intermediate_chain_pem;
    std::string root_private_key_pem;
    std::string intermediate_private_key_pem;
};

struct PkiAuthorityMaterial {
    std::string customer_id;
    std::string root_certificate_pem;
    std::string intermediate_chain_pem;
    std::string root_private_key_pem;
    std::string intermediate_private_key_pem;
};

struct PkiCallerRecord {
    std::string id;
    std::string customer_id;
    std::string workload_taap_id;
    std::string attestation_type;
    std::string issuer;
    std::string subject;
    nlohmann::json attributes = nlohmann::json::object();
    std::string expires_at;
    std::string issued_at;
};

struct PkiCertificateRecord {
    std::string id;
    std::string authority_id;
    std::string customer_id;
    std::string profile;
    std::string subject;
    std::string serial;
    std::string public_value;
    std::string expires_at;
    std::string issued_at;
    std::string renewed_from_id;
    std::string revoked_at;
};

struct NewPkiCertificateRecord {
    std::string customer_id;
    std::string profile;
    std::string subject;
    std::string serial;
    std::string public_value;
    std::string expires_at;
    std::string renewed_from_id;
};

struct NewPkiCallerRecord {
    std::string customer_id;
    std::string workload_taap_id;
    std::string attestation_type;
    std::string issuer;
    std::string subject;
    nlohmann::json attributes = nlohmann::json::object();
    std::string expires_at;
};

struct AgentSession {
    std::string id;
    std::string customer_id;
    std::string durable_agent_taap_id;
    std::string dispatcher_taap_id;
    std::string session_taap_id;
    std::string public_key;
    std::vector<std::string> scopes;
    std::string attestation;
    std::uint64_t transparency_index = 0;
    std::string transparency_root;
    std::int64_t expires_unix = 0; // 0 when the key never expires
    std::string issued_at;
    std::string expires_at; // "never" for a non-expiring key (lake#234)
    std::string revoked_at; // empty while the key is usable

    bool never_expires() const {
        return expires_at == "never";
    }
};

struct NewAgentSession {
    std::string id;
    std::string customer_id;
    std::string durable_agent_taap_id;
    std::string dispatcher_taap_id;
    std::string session_taap_id;
    std::string public_key;
    std::vector<std::string> scopes;
    std::string attestation;
    std::uint64_t transparency_index = 0;
    std::string transparency_root;
    std::int64_t expires_unix = 0;
    std::string issued_at;
    std::string expires_at;
};

struct PkiRecoveryCeremonyRecord {
    std::string id;
    std::string customer_id;
    std::string authority_id;
    int threshold = 0;
    int share_count = 0;
    std::optional<std::string> recovered_at;
    std::string expires_at;
    std::string created_at;
};

struct NewPkiRecoveryCeremonyRecord {
    std::string id;
    std::string customer_id;
    int threshold = 0;
    int share_count = 0;
    std::string expires_at;
};

struct PkiRecoveryShareEvidence {
    int index = 0;
    std::string digest;
};

// Repository transfer in one UTC hour ("YYYY-MM-DDTHH:00:00Z"). It lives in the workspace Zetta
// of the repository's workspace (lake#348, control/metering.hpp); the control store only reads the
// retired metering_usage table for the one-shot copy.
struct MeteringUsage {
    std::string customer_id;
    std::string hour;
    std::uint64_t stored_bytes = 0; // retired: only the legacy rows carry a sampled gauge
    std::uint64_t transfer_in_bytes = 0;
    std::uint64_t transfer_out_bytes = 0;
    std::uint64_t compute_seconds = 0;
};

// One repository build result's compute, counted once per result (lake#349). It lives in the
// workspace Zetta of the repository's workspace; `repository_id` is empty for a copied legacy row,
// which the retired metering_build_records table did not record.
struct MeteredBuild {
    std::string customer_id;
    std::string result_id;
    std::string hour;
    std::uint64_t compute_seconds = 0;
    std::string repository_id;
};

// A row of the retired metering_usage or metering_build_records table with its recorded owner
// (migration 0053). `id` is the row's stable identity, md5 of the customer and the hour (usage) or
// the result id (builds) as a UUID, which pages the copy. `workspace_id` is the live workspace it
// is copied into, or empty with the reason it is not (`needs_selection`, `workspace_deleted`).
struct LegacyMeteringUsage {
    std::string id;
    MeteringUsage usage;
    std::string workspace_id;
    std::string outcome;
};
struct LegacyMeteredBuild {
    std::string id;
    MeteredBuild build;
    std::string workspace_id;
    std::string outcome;
};

nlohmann::json to_json(const User&);
nlohmann::json to_json(const Organization&);
nlohmann::json to_json(const Customer&);
nlohmann::json to_json(const Repository&);
nlohmann::json to_json(const RepositoryRef&);
nlohmann::json to_json(const ForgeRecord&);
nlohmann::json to_json(const Team&);
nlohmann::json to_json(const Membership&);
nlohmann::json to_json(const Invitation&);
nlohmann::json to_json(const PolicyObject&);
nlohmann::json to_json(const Grant&);
nlohmann::json to_json(const DriveShareLink&);
std::string new_uuid_v4();
std::string now_iso8601();
nlohmann::json to_json(const Device&);
nlohmann::json to_json(const Session&);
nlohmann::json to_json(const SsoProvider&);
nlohmann::json to_json(const FederationIssuer&);
nlohmann::json to_json(const ScimUser&);
nlohmann::json to_json(const ScimGroup&);
nlohmann::json to_json(const ProvisioningLog&);

class Store {
public:
    virtual ~Store() = default;
    [[nodiscard]] virtual std::string kind() const = 0; // "postgres" | "memory"
    virtual Result<void> ping() = 0;

    virtual Result<User> create_user(const NewUser&) = 0;
    virtual Result<std::vector<User>> list_users(int limit) = 0;
    virtual Result<User> get_user(const std::string& id) = 0;
    // What people call these TAAP identities: a user's display name (else email) or an
    // organization's display name. Unknown identities are absent from the map (lake#445).
    virtual Result<std::map<std::string, std::string>>
    principal_names(const std::vector<std::string>& taap_ids) = 0;

    virtual Result<Organization> create_organization(const NewOrganization&) = 0;
    virtual Result<std::vector<Organization>> list_organizations(int limit) = 0;
    virtual Result<Organization> get_organization(const std::string& id) = 0;

    virtual Result<Customer> create_customer(const NewCustomer&) = 0;
    virtual Result<std::vector<Customer>> list_customers(int limit,
                                                         const std::string& user_id = {}) = 0;
    virtual Result<Customer> get_customer(const std::string& id) = 0;

    // Refuses a workspace_id that is not a live workspace of customer_id's space (invalid_input).
    virtual Result<Repository> create_repository(const NewRepository&) = 0;
    // customer_id empty = every customer's repositories; a non-empty workspace_id narrows the list
    // to that workspace's repositories (lake#315).
    virtual Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                              int limit,
                                                              const std::string& workspace_id = {}) = 0;
    virtual Result<Repository> get_repository(const std::string& id) = 0;
    virtual Result<Repository> rename_repository(const std::string& id,
                                                 const std::string& name) = 0;
    virtual Result<Repository> set_repository_visibility(const std::string& id,
                                                         const std::string& visibility) = 0;
    virtual Result<Repository> delete_repository(const std::string& id,
                                                 const RepositoryDeletion& deletion) = 0;
    virtual Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) = 0;
    virtual Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                                     const std::string& name,
                                                     const std::string& head) = 0;
    // The retired forge_records table (lake#341), for the one-shot copy into workspace Zettas
    // only. Assigning records each unassigned row's owner in workspace_assignments, as migration
    // 0054 does, for rows written after it, and resolves needs_selection rows whose repository has
    // since been assigned a workspace. Listing pages by id, oldest id first.
    // The central audit events about a workspace's data (decision 52, lake#317), for the
    // one-shot copy into workspace Zettas only. Assigning records the workspace of each event
    // whose action is one of `workspace_actions` and that was not refused for want of authority,
    // as migration 0063 does, for events an earlier lake wrote after it. Listing pages the
    // assigned events in append order after the event `after_id`.
    virtual Result<void>
    assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) = 0;
    virtual Result<std::vector<LegacyAuditEvent>> legacy_audit_events(const std::string& after_id,
                                                                      int limit) = 0;
    virtual Result<void> assign_legacy_forge_records() = 0;
    virtual Result<std::vector<LegacyForgeRecord>>
    legacy_forge_records(const std::string& after_id, int limit) = 0;
    // The retired platform_environments and platform_mirror tables (lake#358, lake#359), for the
    // one-shot copy into workspace Zettas only. Assigning records each unassigned row's owner in
    // workspace_assignments, as migration 0060 does, for rows an earlier lake wrote after it, and
    // resolves needs_selection rows whose repository has since been assigned a workspace;
    // assigning environments also gives each environment's policy object its derived external id.
    // Listing pages by row id, oldest id first.
    virtual Result<void> assign_legacy_platform_environments() = 0;
    virtual Result<std::vector<LegacyPlatformEnvironment>>
    legacy_platform_environments(const std::string& after_id, int limit) = 0;
    virtual Result<void> assign_legacy_platform_mirror() = 0;
    virtual Result<std::vector<LegacyPlatformMirrorRow>>
    legacy_platform_mirror(const std::string& after_id, int limit) = 0;
    // Drive share links live in workspace Zettas (lake#350). The control store answers only the
    // route of a link made before the move, from its token digest; `not_found` otherwise.
    virtual Result<DriveShareLinkRoute> drive_share_link_route(const std::string& token_hash) = 0;
    // The retired drive_share_links table, for the one-shot copy into workspace Zettas only.
    // Assigning records each unassigned row's owner in workspace_assignments and the route of each
    // assigned row, as migration 0048 does, for rows written after it; listing pages by id.
    virtual Result<void> assign_legacy_drive_share_links() = 0;
    virtual Result<std::vector<LegacyDriveShareLink>>
    legacy_drive_share_links(const std::string& after_id, int limit) = 0;

    virtual Result<Team> create_team(const NewTeam&) = 0;
    virtual Result<std::vector<Team>> list_teams(const std::string& organization_id, int limit) = 0;
    virtual Result<Team> get_team(const std::string& id) = 0;

    virtual Result<Membership> create_membership(const NewMembership&) = 0;
    virtual Result<Membership> get_membership(const std::string& id) = 0;
    virtual Result<std::vector<Membership>>
    list_memberships(const std::string& organization_id, const std::string& team_id, int limit) = 0;
    virtual Result<Membership>
    set_member_role(const std::string& id, const std::string& role, const std::string& actor) = 0;
    virtual Result<void> remove_member(const std::string& id, const std::string& actor) = 0;
    virtual Result<Membership> accept_invitation(const AcceptInvitation&) = 0;

    virtual Result<Invitation> create_invitation(const NewInvitation&) = 0;
    virtual Result<std::vector<Invitation>>
    list_invitations(const std::string& organization_id, const std::string& team_id, int limit) = 0;
    virtual Result<Invitation> get_invitation(const std::string& id) = 0;
    // Organizations a person runs themselves (docs/api/organizations.md#self-service).
    virtual Result<OrganizationSpace> create_organization_space(const NewOrganizationSpace&) = 0;
    virtual Result<OrganizationSpace> convert_personal_space(const ConvertPersonalSpace&) = 0;
    // Every organization and team membership held by one user principal, newest first.
    virtual Result<std::vector<Membership>> list_user_memberships(const std::string& user_id,
                                                                  int limit) = 0;
    // Pending, unexpired invitations addressed to an email address or to one of the handles.
    virtual Result<std::vector<Invitation>>
    list_invitee_invitations(const std::string& email,
                             const std::vector<std::string>& handles,
                             int limit) = 0;

    virtual Result<PolicyObject> create_policy_object(const NewPolicyObject&) = 0;
    virtual Result<PolicyObject> get_policy_object(const std::string& id) = 0;
    virtual Result<PolicyObject> find_policy_object(const std::string& object_type,
                                                    const std::string& external_id) = 0;
    virtual Result<std::vector<PolicyObject>> list_policy_objects(const std::string& parent_id,
                                                                  const std::string& object_type,
                                                                  int limit) = 0;
    virtual Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string>& requested_object_ids) = 0;

    virtual Result<Grant> create_grant(const NewGrant&) = 0;
    virtual Result<std::vector<Grant>>
    list_grants(const std::vector<std::string>& object_ids,
                const std::optional<std::string>& principal_taap_id,
                int limit) = 0;

    // An application's routing row (lake#351): its id, space, pool, custom sign-in host and
    // workspace, which find it before its workspace is read. The answers carry no settings; those
    // are the application's record in its workspace (control/application_settings_records.hpp).
    virtual Result<Application> create_application(const NewApplication& value) = 0;
    virtual Result<std::vector<Application>> list_applications(const std::string& customer_id,
                                                               int limit) = 0;
    virtual Result<Application> get_application(const std::string& id) = 0;
    virtual Result<Application> get_application_by_pool(const std::string& pool_id) = 0;
    virtual Result<Application>
    get_application_by_domain(const std::string& custom_sign_in_domain) = 0;
    // The settings columns of the retired auth_applications rows (migration 0059), for the one-shot
    // copy into workspace Zettas only. Assigning records each unassigned row's owner in
    // workspace_assignments (object_table `auth_application_settings`), as the migration does, for
    // rows written after it, and resolves needs_selection rows whose application has since been
    // assigned a live workspace. Only rows that still hold settings are listed, by id, oldest id
    // first.
    virtual Result<void> assign_legacy_application_settings() = 0;
    virtual Result<std::vector<LegacyApplicationSettings>>
    legacy_application_settings(const std::string& after_id, int limit) = 0;
    // The secrets subsystem's envelope for an application credential (lake#352, lake#355): a data
    // key per purpose, wrapped by LAKE_INSTANCE_KEK, encrypts the value with AES-256-GCM. Only the
    // purposes `application.jwt.<application id>` and `application.webhook.<webhook id>` are
    // accepted. The envelope is kept only inside the sealed record of the application's
    // workspace; neither the value nor the envelope is stored here.
    virtual Result<std::string> encrypt_application_credential(const std::string& purpose,
                                                               const std::string& plaintext) = 0;
    virtual Result<std::string> decrypt_application_credential(const std::string& purpose,
                                                               const std::string& envelope) = 0;
    // An application's users and organizations live in the workspace Zetta of the application's
    // workspace (lake#353, lake#354, control/application_user_records.hpp). The store keeps only
    // what is lake's own: the pool's sessions and credentials of a user, which a suspension revokes
    // and a deletion removes, and the retired tables for the one-shot copy. Revoking answers how
    // many sessions it revoked; deleting answers not_found when the application does not exist.
    virtual Result<int> revoke_application_user_sessions(const std::string& application_id,
                                                         const std::string& user_id) = 0;
    virtual Result<void> delete_application_user_credentials(const std::string& application_id,
                                                             const std::string& user_id) = 0;
    // The retired auth_app_users and auth_app_organizations tables (migration 0055). Assigning
    // records each unassigned row's owner in workspace_assignments, as the migration does, for rows
    // written after it, and resolves needs_selection rows whose application has since been assigned
    // a workspace. Listing pages by row id, oldest id first.
    virtual Result<void> assign_legacy_application_directory() = 0;
    virtual Result<std::vector<LegacyApplicationUser>>
    legacy_application_users(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacyApplicationOrganization>>
    legacy_application_organizations(const std::string& after_id, int limit) = 0;
    // The retired auth_app_signing_keys and auth_app_webhooks tables (lake#352, lake#355), for the
    // one-shot copy into workspace Zettas only. Assigning records each unassigned key ring's (one
    // per application) and webhook's owner in workspace_assignments, as migration 0056 does, for
    // rows written after it, and resolves needs_selection rows whose application has since been
    // assigned a workspace. Key rings page by application id, webhooks by id, oldest id first.
    virtual Result<void> assign_legacy_application_credentials() = 0;
    virtual Result<std::vector<LegacyApplicationSigningKeyRing>>
    legacy_application_signing_keys(const std::string& after_application_id, int limit) = 0;
    virtual Result<std::vector<LegacyApplicationWebhook>>
    legacy_application_webhooks(const std::string& after_id, int limit) = 0;
    // The retired auth_app_webhook_events and auth_app_webhook_deliveries tables (lake#356,
    // lake#357), for the one-shot copy into workspace Zettas only. Assigning records each
    // unassigned event's owner in workspace_assignments, as migration 0049 does, for rows written
    // after it, and resolves needs_selection events whose application has since been assigned a
    // workspace. Listing pages by id, oldest id first; a delivery's owner is its event's.
    virtual Result<void> assign_legacy_application_webhook_events() = 0;
    virtual Result<std::vector<LegacyApplicationWebhookEvent>>
    legacy_application_webhook_events(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacyApplicationWebhookDelivery>>
    legacy_application_webhook_deliveries(const std::string& after_id, int limit) = 0;
    // The retired auth_app_authorization_codes table (lake#363), for the one-shot copy into
    // workspace Zettas only: codes are issued and redeemed in the application's workspace
    // (control/application_authorization_codes.hpp). Assigning records the owner of each live
    // (unused, unexpired) code not yet recorded, as migration 0061 does, and resolves
    // needs_selection rows whose application has since been assigned a workspace. Listing returns
    // only live codes, paged by code hash.
    // Recording a code as copied (outcome `copied`, before it is written) keeps the copy from
    // ever writing it again, so a code redeemed in the workspace cannot be made redeemable twice.
    virtual Result<void> assign_legacy_application_authorization_codes() = 0;
    virtual Result<std::vector<LegacyApplicationAuthorizationCode>>
    legacy_application_authorization_codes(const std::string& after_code_hash, int limit) = 0;
    virtual Result<void>
    record_legacy_application_authorization_code_copied(const std::string& code_hash) = 0;

    virtual audit::Result<audit::Identity> current_lake_identity() = 0;
    virtual audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) = 0;
    virtual audit::Result<void> create_lake_identity(const audit::Identity& identity) = 0;
    virtual audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                                     const audit::Identity& replacement) = 0;

    virtual Result<void> append_audit_event(const audit::Event& event) = 0;
    virtual Result<audit::Event> latest_audit_event() = 0;
    virtual Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) = 0;
    virtual Result<void> set_audit_retention_days(const std::string& customer_id, int days) = 0;

    // The retired metering tables (lake#348, lake#349): migration 0053's owner assignment, rerun
    // before each copy for rows an older replica wrote, and the rows a page at a time by id.
    virtual Result<void> assign_legacy_metering() = 0;
    virtual Result<std::vector<LegacyMeteringUsage>>
    legacy_metering_usage(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacyMeteredBuild>>
    legacy_metering_build_records(const std::string& after_id, int limit) = 0;
    virtual Result<std::optional<std::uint64_t>> storage_quota(const std::string& customer_id) = 0;
    virtual Result<std::vector<Workspace>> list_workspaces(const std::string& customer_id) = 0;
    virtual Result<Workspace> get_workspace(const std::string& id) = 0;
    virtual Result<Workspace> create_workspace(const NewWorkspace&) = 0;
    virtual Result<Workspace> update_workspace(const std::string& id, const WorkspacePatch&) = 0;
    // Managed services (docs/control-plane/services.md). Lists the customer's services, oldest
    // first, leaving out deleted ones the operator has finished with.
    virtual Result<std::vector<ManagedService>> list_services(const std::string& customer_id) = 0;
    virtual Result<ManagedService> get_service(const std::string& id) = 0;
    // Creates a service. A default one that already exists is returned unchanged; a live name is a
    // conflict; a customer at max_per_customer live services is refused with quota_exceeded.
    virtual Result<ManagedService> create_service(const NewManagedService&) = 0;
    // Marks a service absent so the operator deletes it. Refuses one already absent.
    virtual Result<ManagedService> withdraw_service(const std::string& id) = 0;
    // Ship apps (docs/control-plane/web-apps.md). The apps row carries desired state from lake and
    // status written by the operator. Acknowledged deletions are removed by the owner loop.
    virtual Result<std::vector<WebApp>> list_web_apps(const std::string& customer_id) = 0;
    virtual Result<WebApp> get_web_app(const std::string& id) = 0;
    virtual Result<WebApp> resolve_web_app_auth_host(const std::string& host,
                                                     const std::string& workspace_id) = 0;
    virtual Result<WebApp> authorize_web_app(const std::string& app_id,
                                             const std::string& user_id) = 0;
    virtual Result<WebApp> authorize_web_app_session(const std::string& host,
                                                     const std::string& workspace_id,
                                                     const std::string& app_id,
                                                     const std::string& user_id) = 0;
    virtual Result<void>
    create_web_app_authorization_code(const WebAppAuthorizationCode& value) = 0;
    virtual Result<WebAppAuthorization> consume_web_app_authorization_code(
        const std::string& host, const std::string& workspace_id, const std::string& code_hash) = 0;
    virtual Result<void> sweep_web_app_authorization_codes() = 0;
    virtual Result<WebApp> create_web_app(const NewWebApp&) = 0;
    virtual Result<WebApp> update_web_app(const std::string& id, const WebAppPatch&) = 0;
    virtual Result<void> purge_deleted_web_apps() = 0;
    // In-cluster image builds (docs/control-plane/builds.md). Lake inserts desired rows; the
    // operator writes status and the hashes of the three credentials it owns.
    virtual Result<std::vector<Build>> list_builds(const std::string& customer_id) = 0;
    // The retired stream_owners table (lake#379), for the one-shot copy into workspace Zettas only.
    // Assigning records each unassigned row's owner in workspace_assignments, as migration 0047
    // does, for rows written after it; listing pages by the row id, oldest id first.
    virtual Result<void> assign_legacy_stream_owners() = 0;
    virtual Result<std::vector<LegacyStreamOwner>> legacy_stream_owners(const std::string& after_id,
                                                                        int limit) = 0;
    virtual Result<std::vector<AgentTaskStreamScope>>
    list_agent_task_stream_scopes(const std::string& customer_id) = 0;
    virtual Result<Build> get_build(const std::string& id) = 0;
    virtual Result<Build> create_build(const NewBuild&) = 0;
    virtual Result<Build> cancel_build(const std::string& id) = 0;
    // Stores (docs/control-plane/storage.md). Lists the customer's stores, oldest first, leaving
    // out deleted ones the operator has finished with; an empty workspace_id lists every
    // workspace's.
    virtual Result<std::vector<ManagedStore>>
    list_stores(const std::string& customer_id, const std::string& workspace_id = {}) = 0;
    virtual Result<ManagedStore> get_store(const std::string& id) = 0;
    // Creates a store; a live name is a conflict. The provider table is the caller's to check.
    virtual Result<ManagedStore> create_store(const NewManagedStore&) = 0;
    // Marks a store absent so the operator deletes it and its data. Refuses one already absent.
    virtual Result<ManagedStore> withdraw_store(const std::string& id) = 0;
    // Agent launches (lake#231). Writing one NOTIFYs lake_services, as services and stores do.
    virtual Result<AgentLaunch> create_agent_launch(const NewAgentLaunch&) = 0;
    virtual Result<AgentLaunch> get_agent_launch(const std::string& id) = 0;
    // Newest first; an empty agent_id lists every launch of the customer, an empty workspace_id
    // every workspace's.
    virtual Result<std::vector<AgentLaunch>>
    list_agent_launches(const std::string& customer_id, const std::string& agent_id, int limit,
                        const std::string& workspace_id = {}) = 0;
    virtual Result<AgentLaunch> withdraw_agent_launch(const std::string& id) = 0;
    // Withdraws every present launch the operator has not reported on within `seconds` of its
    // creation, recording `reason` as its stop_reason (lake#442). Returns how many it withdrew.
    virtual Result<std::size_t> expire_unstarted_agent_launches(std::int64_t seconds,
                                                                const std::string& reason) = 0;
    // Cron schedules: any number per agent. Creating or withdrawing one NOTIFYs lake_services.
    virtual Result<AgentCronSchedule> create_agent_cron_schedule(const NewAgentCronSchedule&) = 0;
    virtual Result<AgentCronSchedule> get_agent_cron_schedule(const std::string& id) = 0;
    // Present schedules, oldest first; an empty agent_id lists every agent's.
    virtual Result<std::vector<AgentCronSchedule>>
    list_agent_cron_schedules(const std::string& customer_id, const std::string& agent_id,
                              const std::string& workspace_id = {}) = 0;
    // Marks a schedule absent so the operator deletes its CronJob. Refuses one already absent.
    virtual Result<AgentCronSchedule> withdraw_agent_cron_schedule(const std::string& id) = 0;
    // The runs the schedules' CronJobs started, as launches (trigger "schedule"), newest first.
    virtual Result<std::vector<AgentLaunch>> list_agent_cron_runs(const std::string& customer_id,
                                                                  const std::string& agent_id,
                                                                  int limit,
                                                                  const std::string& workspace_id = {}) = 0;
    // Background agent turns (lake#269, docs/api/agent-bus.md): the dispatch half only. The
    // request, messages and answer are records in the task's workspace (lake#374, lake#375).
    // Creating keeps `id` when the caller made one (lake writes the request record under it
    // first) and stores nothing of `body`, `message` or `result`.
    virtual Result<AgentTask> create_agent_task(const AgentTask&) = 0;
    virtual Result<AgentTask> get_agent_task(const std::string& id) = 0;
    // A space's newest task of one kind (a project's pipeline build is kind "build:<project>");
    // not_found when there is none.
    virtual Result<AgentTask> latest_agent_task(const std::string& customer_id,
                                                const std::string& kind,
                                                const std::string& workspace_id) = 0;
    // The newest earlier turn in one conversation; not_found when this is its first turn.
    virtual Result<AgentTask> latest_agent_conversation_task(const std::string& customer_id,
                                                             const std::string& conversation_id,
                                                             const std::string& agent,
                                                             const std::string& kind,
                                                             const std::string& workspace_id) = 0;
    // The task's agent claims it: Running, with the SHA-256 of the credential lake just handed
    // it and where on agent-events its events will start. Exactly one claim per attempt succeeds;
    // another is a conflict, as is a claim by a different agent.
    virtual Result<AgentTask> claim_agent_task(const std::string& id,
                                               const std::string& agent,
                                               const std::string& credential_sha256,
                                               std::uint64_t events_from = 0,
                                               int lease_seconds = 600) = 0;
    // Activity by the current task credential renews its claim. Other credentials conflict.
    virtual Result<void> renew_agent_task_lease(const std::string& id,
                                                const std::string& credential_sha256) = 0;
    // Retry notices remain pending until their workspace publication succeeds.
    virtual Result<void> mark_agent_task_notice_published(const std::string& id,
                                                          int attempts,
                                                          const std::string& phase) = 0;
    // What the claiming agent reported: Running (renewing its lease), or Succeeded / Failed.
    // Refused for a task that agent has not claimed or that already finished, so the first
    // terminal event wins. Clears lake's notice. The message and answer are the caller's to
    // record in the task's workspace once this accepted the event.
    virtual Result<AgentTask> record_agent_task_event(const std::string& id,
                                                      const std::string& agent,
                                                      const std::string& phase) = 0;
    // Lake's waiting deadline: fail only an unclaimed task; a concurrent claim is left alone.
    // `notice` is lake's own text for it, never what a person or an agent wrote.
    virtual Result<void> fail_agent_task(const std::string& id, const std::string& notice) = 0;
    virtual Result<void> mark_agent_task_published(const std::string& id) = 0;
    virtual Result<StaleAgentTasks> stale_agent_tasks(std::chrono::seconds republish_after) = 0;
    // The retired content columns of agent_tasks and agent_task_status (lake#374, lake#375), for
    // the one-shot copy into workspace Zettas only. Assigning records the owner of each task with
    // content that has none yet in workspace_assignments, as migration 0062 does, for rows an
    // earlier lake wrote after it; listing pages by task id, oldest id first.
    virtual Result<void> assign_legacy_agent_tasks() = 0;
    virtual Result<std::vector<LegacyAgentTask>> legacy_agent_tasks(const std::string& after_id,
                                                                    int limit) = 0;
    // The retired opportunities table (lake#376), for the one-shot copy into workspace Zettas
    // only. Assigning records each unassigned row's owner in workspace_assignments, as migration
    // 0046 does, for rows written after it; listing pages by id, oldest id first.
    virtual Result<void> assign_legacy_opportunities() = 0;
    virtual Result<std::vector<LegacyOpportunity>> legacy_opportunities(const std::string& after_id,
                                                                        int limit) = 0;
    virtual Result<std::uint64_t> set_storage_quota(const std::string& customer_id,
                                                    std::uint64_t quota_bytes) = 0;

    virtual Result<PkiAuthorityRecord> create_pki_authority(const NewPkiAuthorityRecord& value,
                                                            const std::string& instance_kek) = 0;
    virtual Result<PkiAuthorityMaterial> get_pki_authority(const std::string& customer_id,
                                                           const std::string& instance_kek) = 0;
    virtual Result<PkiCallerRecord> create_pki_caller(const NewPkiCallerRecord& value) = 0;
    virtual Result<std::vector<PkiCallerRecord>> list_pki_callers(const std::string& customer_id,
                                                                  int limit) = 0;
    virtual Result<PkiCertificateRecord>
    create_pki_certificate(const NewPkiCertificateRecord& value) = 0;
    virtual Result<PkiCertificateRecord> get_pki_certificate(const std::string& id) = 0;
    virtual Result<PkiCertificateRecord> revoke_pki_certificate(const std::string& id) = 0;
    virtual Result<AgentSession> create_agent_session(const NewAgentSession& value) = 0;
    // Marks one agent session key revoked; a revoked key never verifies again (lake#234).
    virtual Result<AgentSession> revoke_agent_session(const std::string& customer_id,
                                                      const std::string& id) = 0;
    // The customer's opt-in to credentials that never expire (lake#234), off by default.
    virtual Result<bool>
    customer_allows_non_expiring_credentials(const std::string& customer_id) = 0;
    virtual Result<void> set_customer_non_expiring_credentials(const std::string& customer_id,
                                                               bool allowed) = 0;
    virtual Result<AgentSession> get_agent_session(const std::string& customer_id,
                                                   const std::string& id) = 0;
    virtual Result<PkiRecoveryCeremonyRecord>
    create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& value) = 0;
    virtual Result<PkiRecoveryCeremonyRecord>
    get_pki_recovery_ceremony(const std::string& customer_id, const std::string& ceremony_id) = 0;
    virtual Result<PkiRecoveryCeremonyRecord>
    complete_pki_recovery_ceremony(const std::string& customer_id,
                                   const std::string& ceremony_id,
                                   const std::vector<PkiRecoveryShareEvidence>& evidence) = 0;

    // Secrets (docs/api/secrets.md, lake#328-#332). Mounts, secrets, versions, named keys and data
    // keys live in the mount's workspace Zetta (api/secret_records_api.hpp). The control store
    // keeps only what is multitenancy or a root of the key hierarchy: each mount's content-free
    // route and policy objects, and the customer KEK that wraps every data key.

    // Records where a new mount lives and adds organization -> environment -> secret_mount to the
    // policy graph. `invalid_input` at /organization_id when the organization is not inside the
    // customer, at /workspace_id when the workspace is not a live workspace of that customer.
    virtual Result<SecretMountRoute> create_secret_mount_route(const SecretMountRoute& value) = 0;
    virtual Result<SecretMountRoute> get_secret_mount_route(const std::string& mount_id) = 0;
    // A customer's routes, newest first; `workspace_id` empty: every workspace.
    virtual Result<std::vector<SecretMountRoute>>
    list_secret_mount_routes(const std::string& customer_id, const std::string& workspace_id) = 0;

    // The customer KEK wraps and unwraps data keys without leaving the store. Wrapping creates the
    // customer's KEK on first use and answers the version it used; unwrapping accepts the current
    // version and, during a rotation, the previous one.
    virtual Result<WrappedDataKey> wrap_secret_data_key(const std::string& customer_id,
                                                        const std::string& owner_id,
                                                        std::uint64_t key_version,
                                                        const std::string& raw_key,
                                                        const std::string& instance_kek) = 0;
    virtual Result<std::string> unwrap_secret_data_key(const std::string& customer_id,
                                                       const std::string& owner_id,
                                                       std::uint64_t key_version,
                                                       const WrappedDataKey& wrapped,
                                                       const std::string& instance_kek) = 0;
    virtual Result<CustomerKekState> customer_kek_state(const std::string& customer_id) = 0;
    // A new current KEK; the replaced one is kept as `previous_version` until
    // retire_customer_kek. Rewraps, in the same transaction, the customer's PKI authorities and the
    // rows of the retired secrets_data_keys table (which the one-shot copy still reads). `conflict`
    // while an earlier rotation's previous version is still kept.
    virtual Result<CustomerKekRotation> rotate_customer_kek(const std::string& customer_id,
                                                            const std::string& instance_kek) = 0;
    // Forgets `previous_version` once no workspace data key is wrapped by it.
    virtual Result<void> retire_customer_kek(const std::string& customer_id,
                                             std::uint64_t previous_version) = 0;

    // Dynamic leases live in their mount's workspace Zetta (lake#362). The control store keeps
    // each lease's content-free route (secret_lease_routes, migration 0058), which is also the
    // expiry loop's due index: the loop asks it which leases are due instead of reading every
    // workspace.
    //
    // A new route for a lease of a routed mount, in that mount's workspace and space. A lease
    // that already has a route answers it unchanged. `invalid_input` at /mount_id when the mount
    // has no route or the route names another workspace.
    virtual Result<SecretLeaseRoute> record_secret_lease_route(const SecretLeaseRoute& value) = 0;
    virtual Result<SecretLeaseRoute> get_secret_lease_route(const std::string& lease_id) = 0;
    // Moves a lease's due instant; 0 settles it. A settled route is never made due again.
    virtual Result<void> reschedule_secret_lease(const std::string& lease_id,
                                                 std::int64_t due_us) = 0;
    // The route of a lease whose workspace record was never written.
    virtual Result<void> delete_secret_lease_route(const std::string& lease_id) = 0;
    // Up to `limit` routes due now, earliest first, each held for `hold_seconds` (its due instant
    // moved that far ahead) so that another replica's loop does not take it meanwhile; a loop
    // that stops leaves them to be taken again once the hold passes.
    virtual Result<std::vector<SecretLeaseRoute>> claim_due_secret_leases(int limit,
                                                                          int hold_seconds) = 0;

    // The retired secrets tables, for the one-shot copy only. assign_legacy_secrets records each
    // legacy mount's owner (migration 0057's rule) and the route of every assigned mount; the
    // listings page by row id.
    virtual Result<void> assign_legacy_secrets() = 0;
    virtual Result<std::vector<LegacySecretMount>> legacy_secret_mounts(const std::string& after_id,
                                                                        int limit) = 0;
    virtual Result<std::vector<LegacySecretEntry>> legacy_secret_entries(const std::string& after_id,
                                                                         int limit) = 0;
    virtual Result<std::vector<LegacySecretVersion>>
    legacy_secret_versions(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacyEncryptionKey>>
    legacy_encryption_keys(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacySecretDataKey>>
    legacy_secret_data_keys(const std::string& after_id, int limit) = 0;
    virtual Result<std::vector<LegacyDynamicSecretLease>>
    legacy_dynamic_secret_leases(const std::string& after_id, int limit) = 0;

    // Identity core. All bearer and cookie values are resolved here on every
    // request so a revoked session or device is rejected immediately.
    virtual Result<IssuedSession> sign_up_password(const NewPasswordUser&) = 0;
    virtual Result<IssuedSession> sign_in_password(const PasswordSignIn&) = 0;
    virtual Result<IssuedSession> sign_in_taap(const TaapSignIn&) = 0;
    virtual Result<MfaEnrollment> enroll_mfa(const std::string& user_id,
                                             const std::string& pool_id) = 0;
    virtual Result<void> disable_mfa(const std::string& user_id,
                                     const std::string& pool_id,
                                     const std::string& code) = 0;
    virtual Result<IssuedSession> sign_in_totp(const PasswordSignIn&) = 0;
    virtual Result<WebAuthnChallenge> begin_passkey_registration(const std::string& user_id,
                                                                 const std::string& pool_id,
                                                                 const std::string& rp_id,
                                                                 const std::string& origin) = 0;
    virtual Result<void> register_passkey(const PasskeyRegistration&) = 0;
    virtual Result<WebAuthnChallenge> begin_passkey_sign_in(const std::string& pool_id,
                                                            const std::string& rp_id,
                                                            const std::string& origin) = 0;
    virtual Result<IssuedSession> sign_in_passkey(const PasskeyAssertion&) = 0;
    virtual Result<IssuedSession> sign_in_oauth(const OAuthIdentity&) = 0;
    // Signs in the lake-pool user linked to this Clerk subject. With no link, it links the lake
    // user with the same email; with neither, it creates the user with the custodial identity
    // and provisions their personal customer, as password sign-up does.
    virtual Result<IssuedSession> sign_in_clerk(const ClerkIdentity&) = 0;
    virtual Result<void> register_oauth_provider(const OAuthProvider&) = 0;
    virtual Result<OAuthProvider> get_oauth_provider(const std::string& pool_id,
                                                     const std::string& provider) = 0;
    virtual Result<void> begin_oauth(const OAuthFlow&) = 0;
    virtual Result<OAuthFlow> consume_oauth(const std::string& provider,
                                            const std::string& state) = 0;
    virtual Result<IssuedSession> create_token(const NewToken&) = 0;
    virtual Result<std::vector<Session>> list_sessions(const std::string& user_id,
                                                       const std::string& pool_id) = 0;
    virtual Result<void> revoke_session(const std::string& user_id,
                                        const std::string& session_id) = 0;
    virtual Result<std::vector<Device>> list_devices(const std::string& user_id) = 0;
    virtual Result<void> revoke_device(const std::string& user_id,
                                       const std::string& device_id) = 0;
    virtual Result<Principal> context_for(const std::string& secret) = 0;

    virtual Result<OfflineGrant> issue_offline_grant(const NewOfflineGrant& grant) = 0;
    virtual Result<std::vector<OfflineGrant>> list_offline_grants(const std::string& user_id) = 0;
    virtual Result<OfflineGrant> get_offline_grant(const std::string& id) = 0;
    virtual Result<OfflineGrant> revoke_offline_grant(const std::string& user_id,
                                                      const std::string& id) = 0;

    virtual Result<IssuedSession> recover_with_code(const std::string& pool_id,
                                                    const std::string& email,
                                                    const std::string& code,
                                                    const std::string& device_name) = 0;
    virtual Result<RecoveryAttempt> begin_email_recovery(const std::string& pool_id,
                                                         const std::string& email,
                                                         int cooling_seconds) = 0;
    virtual Result<RecoveryAttempt> begin_admin_recovery(const std::string& organization_id,
                                                         const std::string& user_id,
                                                         const std::string& requested_by) = 0;
    virtual Result<RecoveryAttempt> get_recovery_attempt(const std::string& recovery_id) = 0;
    virtual Result<RecoveryAttempt> approve_admin_recovery(const std::string& recovery_id,
                                                           const std::string& approved_by) = 0;
    virtual Result<IssuedSession> complete_recovery(const std::string& recovery_id,
                                                    const std::string& token,
                                                    const std::string& device_name) = 0;
    virtual Result<std::vector<RecoveryDelivery>> claim_recovery_deliveries(int limit) = 0;
    virtual Result<std::vector<RecoveryNotification>>
    list_recovery_notifications(const std::string& user_id) = 0;

    virtual Result<RecoveryContact> configure_recovery_contact(const RecoveryContact& contact) = 0;
    virtual Result<RecoveryContact> get_recovery_contact(const std::string& repository_id) = 0;

    virtual Result<SsoProvider> register_sso_provider(const NewSsoProvider&) = 0;
    virtual Result<std::vector<SsoProvider>> list_sso_providers(const std::string& organization_id,
                                                                int limit) = 0;
    virtual Result<SsoProvider> get_sso_provider(const std::string& id) = 0;
    // Internal callback lookup. This is never rendered through an API response.
    virtual Result<OAuthProvider> get_sso_oidc_provider(const std::string& id) = 0;
    virtual Result<SsoProvider> authenticate_scim(const std::string& bearer_token) = 0;
    virtual Result<SsoProvider> test_sso_provider(const std::string& id) = 0;
    virtual Result<FederationIssuer> create_federation_issuer(const NewFederationIssuer&) = 0;
    virtual Result<std::vector<FederationIssuer>>
    list_federation_issuers(const std::string& organization_id, int limit) = 0;
    virtual Result<bool> allows_federation_claim(const std::string& organization_id,
                                                 const std::string& issuer,
                                                 const std::string& claim) = 0;
    virtual Result<IssuedSession> sign_in_sso(const SsoIdentity&) = 0;
    virtual Result<std::vector<ScimUser>> list_scim_users(const std::string& provider_id,
                                                          int limit) = 0;
    virtual Result<ScimUser> get_scim_user(const std::string& provider_id,
                                           const std::string& resource_id) = 0;
    virtual Result<ScimUser> upsert_scim_user(const NewScimUser&) = 0;
    virtual Result<ScimUser> delete_scim_user(const std::string& provider_id,
                                              const std::string& resource_id) = 0;
    virtual Result<std::vector<ScimGroup>> list_scim_groups(const std::string& provider_id,
                                                            int limit) = 0;
    virtual Result<ScimGroup> get_scim_group(const std::string& provider_id,
                                             const std::string& resource_id) = 0;
    virtual Result<ScimGroup> upsert_scim_group(const NewScimGroup&) = 0;
    virtual Result<void> delete_scim_group(const std::string& provider_id,
                                           const std::string& resource_id) = 0;
    virtual Result<std::vector<ProvisioningLog>>
    list_sso_provisioning_logs(const std::string& provider_id, int limit) = 0;
};

} // namespace lake::control
