#pragma once
#include <algorithm>
#include <array>
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
#include "lake/control/network.hpp"
#include "lake/control/offline_recovery.hpp"
#include "lake/control/secrets.hpp"
#include "lake/control/store_mounts.hpp"
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
    // `description` is never read from the control store: it is the repository's settings record
    // in its workspace (lake#315 part 2), filled in by get_repository and list_repositories.
    std::string id, customer_id, name, description, visibility, deleted_at, retention_until,
        created_at, updated_at;
    // The workspace the repository belongs to (migration 0051, lake#315): set on create and never
    // changed. Empty only for a repository the migration recorded as needs_selection.
    std::string workspace_id;
    // The id liquid gave the repository (`repository::id()`, 32 lowercase hex digits), which a
    // checkout cloned from it shares and `liquid deploy` sends as the deployment's `repository`
    // (migration 0088, lake#805). Empty until lake has read it: the platform sync fills it in
    // when a row names an id it does not know, and every push records it again.
    std::string liquid_id;
};
// A repository's settings record (lake#315 part 2, control/repository_records.hpp): its
// description, and later settings, in the workspace Zetta of the repository's workspace.
struct RepositorySettings {
    std::string repository_id;
    std::string customer_id;
    std::string description;
    std::string created_at;
    std::string updated_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
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
    // `description` must be empty: a repository's description is its settings record in its
    // workspace Zetta (lake#315 part 2, control/repository_records.hpp), and the store refuses one.
    // The column it used to fill is dropped once the copy is verified.
    std::string customer_id, name, description,
        visibility; // visibility empty = private
    // Required: a live workspace of customer_id's space (migration 0051, lake#315).
    std::string workspace_id;
    // The new repository's id; empty = a new UUID. create_repository writes the settings record
    // under this id before the row exists, so a refused row leaves nothing behind.
    std::string id;
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
    // The managed key lake minted for a lake-pool sign-up without an identity document (lake#762,
    // decision 65); empty when the person brought their own document.
    std::string custodial_taap_id, custodial_public_key, custodial_wrapped_seed;
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
// What a Clerk user event did to Lake (lake#765, docs/api/clerk.md#webhooks).
struct ClerkUserProvisioned {
    std::string user_id, taap_id;
    bool created = false; // a new account, with a managed key and a personal customer
    bool linked = false;  // the subject was linked to an account found by its verified email
};
// Which key an account signs with (lake#762, docs/api/identity.md#your-own-identity). `managed`:
// lake minted and holds the key. `own`: the person's own liquid identity, either registered at
// sign-up (no managed key ever existed) or linked later as the managed key's successor.
struct AccountIdentity {
    std::string user_id;
    std::string taap_id; // the account's identifier, which grants and audit are keyed on
    std::string key;     // "managed" or "own"
    std::string own_taap_id;
    std::string own_document_hash;
    std::string linked_at;
    // The managed key's public half, kept after its seed is destroyed so the link verifies.
    std::string managed_public_key;
    bool managed_seed_held = false;
    std::string managed_seed_destroyed_at;
    nlohmann::json link_document; // null unless linked
};
// The account's managed key with its wrapped seed, read only to sign the link.
struct ManagedKey {
    std::string user_id, taap_id, public_key, wrapped_seed;
};
// A pending "use my own identity": the person's verified public document and the statement
// they must sign with that key, bound to one account and expiring (identity_link_challenges).
struct IdentityLinkChallenge {
    std::string challenge, user_id, account_taap_id, own_taap_id, own_document, own_document_hash;
    nlohmann::json statement;
    std::string expires_at;
};
// The committed rotation: both signatures are in `link_document`.
struct IdentityLink {
    std::string user_id, account_taap_id, own_taap_id, own_document, own_document_hash;
    nlohmann::json link_document;
};
// A service a customer runs on the Lake's cluster (docs/control-plane/services.md): lake writes
// the desired fields, the operator the status fields.
// A usage limit on one dimension of a customer's usage (migration 0072, lake#564,
// docs/api/metering.md#limits): `limit` in the dimension's unit, per UTC calendar month for a
// counted dimension, and a warning from `warn_percent` of it. Storage's limit is the enforced
// repository storage quota (customer_storage_quotas); every other limit warns and is not enforced.
// A content-free number about the account, like the storage quota; never a price.
struct UsageLimit {
    std::string customer_id, dimension;
    std::uint64_t limit = 0;
    int warn_percent = 80;
    std::string updated_at;
};
// Billing records (migration 0073, lake#564, decision 56, docs/api/billing.md): the price list and
// the invoices lake issues. Control-plane multitenancy; usage stays in each workspace Zetta and
// only the quantity billed per dimension is frozen on a line. Amounts are integer millionths of the
// currency's unit. Lake is priced by usage only (lake#635): each publication of the price list is
// stored as one BillingPlan row (no fee, nothing included); there are no plans to choose.
struct BillingPrice {
    std::string dimension; // a usage_dimensions() name
    std::uint64_t unit_micros = 0;
    std::uint64_t per = 1;      // units of the dimension's unit `unit_micros` buys
    std::uint64_t included = 0; // units a month the plan includes
    // A class within the dimension (a GPU class such as `h100`, migration 0075, lake#643): a rate
    // for that class alone. Empty: the dimension's default rate, for every class without its own.
    std::string class_name;
};
struct BillingPlan {
    std::string id, name, description, currency = "USD";
    std::uint64_t base_micros = 0; // the fee for one month
    bool active = true;
    int position = 0;
    std::vector<BillingPrice> prices;
    std::string created_at, updated_at;
};
struct InvoiceLine {
    std::string kind; // plan or usage
    std::string dimension, description, unit;
    std::uint64_t quantity = 0, included = 0, billable = 0, unit_micros = 0, per = 1,
                  amount_micros = 0;
    std::string class_name; // the class this line bills (migration 0075); empty for the rest
};
struct Invoice {
    std::string id;
    std::uint64_t number = 0;
    std::string customer_id, period, plan_id, plan_name, currency;
    std::uint64_t total_micros = 0;
    std::string state = "due"; // due, paid or void
    std::string provider, provider_reference, instructions, note;
    std::string issued_at, paid_at, voided_at;
    std::vector<InvoiceLine> lines;
    // Where the account pays (Stripe's hosted invoice page) and the provider's own state of the
    // invoice (`open`, `paid`, `payment_failed`, `void`), migration 0074. Empty for manual.
    std::string payment_url, provider_status;
};
// A payment provider's notice (a Stripe webhook event, migration 0074), applied once per
// (provider, event_id): `state` moves a due invoice (`paid`, `void`; empty keeps it) and
// `provider_status` is kept on it.
struct PaymentEvent {
    std::string provider, event_id, event_type, reference, state, provider_status, note;
};
struct PaymentEventResult {
    bool duplicate = false; // the event id was applied before; nothing changed
    bool matched = false;   // an invoice of this provider has `reference`
    bool changed = false;   // the invoice's state or provider_status changed
    Invoice invoice;        // the invoice after the event, when matched
};
// At most `bytes` bytes of `text`, never cut inside a UTF-8 sequence, so a bounded column is never
// sent invalid UTF-8 (an order's event detail and email error, lake#833).
inline std::string utf8_prefix(std::string_view text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return std::string(text);
    }
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(text.substr(0, end));
}
// An order from the apex order sheet (lake#831, migration 0089, docs/api/orders.md): what a person
// asked for, how it is paid, and where it stands. Orders are multitenancy and billing, so they live
// in the control plane; the operator reads `paid` rows and writes `state`, `host`,
// `failure_reason`, `claimed_by` and `claimed_at` (orama-operator#634). Nullable columns read as
// empty strings here.
struct CloudOrder {
    std::string id, created_at, updated_at;
    std::string offer; // private_warehouse | dedicated_cluster | on_prem | shared
    std::string path;  // checkout | order_form
    std::string cloud_provider, region, contact_name, contact_email, company, details;
    int quantity = 1;
    std::int64_t amount_cents = 0; // the monthly fee for `quantity`
    std::string currency = "usd";
    std::int64_t included_credit_cents = 0; // the usage credit the fee includes, each month
    std::string stripe_checkout_session_id, stripe_customer_id, stripe_subscription_id;
    // submitted | pending_payment | paid | provisioning | ready | failed | cancelled
    std::string state;
    std::string host, failure_reason, claimed_by, claimed_at;
    std::string organization_id; // set when the person ordering chose an organization (lake#832)
    // Email delivery (docs/api/orders.md#email): the last state each recipient was told about,
    // the failed attempts since the last delivery, the last failure, and when to try again.
    std::string support_emailed_state, customer_emailed_state;
    int email_attempts = 0;
    std::string email_error, email_retry_at;
};
// A new order: `path` checkout starts it in `pending_payment`, order_form in `submitted`.
struct NewCloudOrder {
    std::string offer, path, cloud_provider, region, contact_name, contact_email, company, details;
    int quantity = 1;
    std::int64_t amount_cents = 0;
    std::string currency = "usd";
    std::int64_t included_credit_cents = 0;
    std::string organization_id;
    std::string email_error; // why the emails cannot go yet (LAKE_RESEND_API_KEY unset), or ""
};
// One state change of an order made by `actor` (lake | stripe | operator), only from `from`
// (conflict otherwise). `to` equal to `from` changes only the non-empty fields below and records no
// event. Every change of state is one cloud_order_events row.
struct CloudOrderTransition {
    std::string id, from, to, actor, detail;
    std::string checkout_session_id, host, failure_reason;
};
// A Stripe Checkout notice for an order, applied once per (provider, event_id) through
// billing_payment_events: `state` paid or cancelled, from pending_payment only.
struct CloudOrderPayment {
    std::string provider, event_id, event_type, checkout_session_id, order_id, state;
    std::string customer_id, subscription_id, detail;
};
struct CloudOrderPaymentResult {
    bool duplicate = false; // the event id was applied before; nothing changed
    bool matched = false;   // an order has this Checkout Session
    bool changed = false;   // the order moved
    CloudOrder order;       // the order after the notice, when matched
};
// One row of cloud_order_events: the order's history, written by lake, Stripe's notices and the
// operator alike. `audited_at` empty until lake has put it in the central audit log.
struct CloudOrderEvent {
    std::int64_t id = 0;
    std::string order_id, at, from_state, to_state, actor, detail, audited_at;
};
// What one email attempt for an order did: `recipient` support or customer, about `state`.
// Delivered: the recipient's emailed state becomes `state`. Not delivered: the attempt is counted,
// `error` kept, and the next attempt waits `retry_seconds`.
struct CloudOrderEmailOutcome {
    std::string id, recipient, state;
    bool delivered = false;
    std::string error;
    int retry_seconds = 60;
};
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
// The resources a workload is scheduled with (migration 0075, lake#643,
// docs/api/metering.md#resources), written only by the Orama operator: the totals of the
// workload's scheduled pods, each container's request or, without one, its limit. `pods` is how
// many are scheduled now; a build's or a job's row keeps what its pods ran with after they finish.
struct WorkloadResources {
    std::string object_kind; // workspace, service, store, app, build, agent_launch, agent_cron_run
    std::string object_id;
    int pods = 0;
    std::uint64_t cpu_millis = 0;
    std::uint64_t memory_bytes = 0;
    int gpus = 0;
    std::string gpu_class; // l4, a10g, a100, h100, ...; empty when unknown
    std::uint64_t vram_bytes = 0;
    std::string observed_at;
};
// Where a workspace's MCP connections need Lake outside a request (migration 0079, lake#684,
// lake#685, docs/api/mcp-connections.md): a connection whose streams the stream worker drives,
// and the digest of the tools it re-exports through Lake's MCP. Ids and a digest only: the
// connection itself is a record in its workspace Zetta. A row with neither is not kept.
struct McpConnectionDispatch {
    std::string customer_id;
    std::string workspace_id;
    std::string connection_id;
    bool streams = false;
    // The SHA-256 (hex) of the tools/list entries the connection re-exports; empty when it
    // re-exports none.
    std::string exported_digest;
    std::string updated_at;
};
// An app's traffic through the ingress in one UTC hour (migration 0081, lake#661,
// docs/api/metering.md#app-traffic), written only by the Orama operator from the ingress
// controller's byte counters: `in_bytes` clients sent to the app, `out_bytes` it sent back. The
// operator adds each poll's increase to its hour's row; lake charges a closed hour once.
struct AppTransfer {
    std::string app_id;
    std::string workspace_id;
    std::string hour; // "YYYY-MM-DDTHH:00:00Z"
    std::uint64_t in_bytes = 0;
    std::uint64_t out_bytes = 0;
    std::string observed_at;
};
struct Workspace {
    std::string id, customer_id, name, environment;
    std::string desired = "running";
    std::int64_t generation = 1;
    std::string phase;
    std::int64_t observed_generation = 0;
    std::string message, namespace_name, zetta_service_id, created_at, updated_at;
    // The desired size of each of the workspace Zetta's volumes, in GiB (migration 0069,
    // operator#259); it never decreases. The operator reports the smallest claim capacity it sees
    // (`storage_gi_observed`), `storage_phase` (Pending | Resizing | Ready | Failed) and why.
    std::int32_t storage_gi = 5;
    std::int32_t storage_gi_observed = 0;
    std::string storage_phase, storage_message;
    // When the workspace's repositories moved into its own liquid store in its workspace Zetta
    // (lake#380, migration 0067, move_workspace_store `switch`); empty while they are still served
    // from the space's store on lake's volume.
    std::string store_moved_at;
};
struct NewWorkspace {
    std::string customer_id, name, environment, created_by_taap_id;
};
struct WorkspacePatch {
    std::optional<std::string> name, environment, desired;
};
// Deleting a workspace cascades (decision 54 S12, lake#514, docs/api/workspaces.md#deletion).
// One object it destroyed: a repository (`live`, or `tombstoned` inside its retention, when the
// workspace was deleted), an app, a build it cancelled, or a tenant image repository (`name` is
// the OCI repository, `<space>/<name>` or `<space>/cache/<name>`, and `id` is empty).
struct WorkspaceDeletionItem {
    std::string id, name, state;
};
// What one workspace deletion destroyed, recorded in the transaction that deleted the workspace
// (migration 0065, `workspace_deletions`). `completed_at` is empty until lake has removed what
// lives outside PostgreSQL (tenant image tags, search segments) and recorded one central audit
// event per destroyed object; until then the owner loop and a repeated delete_workspace resume it.
struct WorkspaceDeletion {
    std::string workspace_id, customer_id, workspace_name, requested_by, requested_at,
        completed_at;
    std::vector<WorkspaceDeletionItem> repositories, apps, builds, images;
};
struct WorkspaceDeletionRequest {
    std::string workspace_id, requested_by;
    // The tenant image repositories the instance registry still holds for the workspace (written
    // before lake#383 and not yet copied), resolved by the caller. The workspace's routes
    // (oci_repository_routes) are added and deleted by the store in the cascade transaction.
    std::vector<std::string> images;
};
// Deleting an organization (lake#645, docs/api/organizations.md#deleting-an-organization). It is
// refused while its space holds a workspace that is not deleted (`live_workspaces` names them).
// Otherwise one transaction removes its teams, memberships, invitations, SSO providers,
// federation issuers, recovery attempts and secret mount routes, and the policy objects of its
// space and of itself (their children and every grant on them go with them), and marks the
// organization and its space deleted. The two rows stay, marked, because the central audit chain
// names the space; they are never listed or read again, and the handle stays retired. `counts`
// says how many of each went (a dry run: would go).
struct OrganizationDeletion {
    Organization organization;
    std::string customer_id, customer_name; // empty when it has no space
    std::vector<Workspace> live_workspaces;
    std::map<std::string, std::int64_t> counts;
    std::string deleted_at; // empty for a dry run
};
// Where a tenant image repository lives (lake#383, decision 54 S4-S6, migration 0068): the
// content-free central route from `<space>/<name>` (the space by its customer id, `name` the path
// below it, `<image>` or `cache/<image>`) to the workspace whose Zetta holds the repository's
// manifests and tags. It holds no manifest, digest or tag. The route never changes: its workspace,
// space and name are immutable, and it goes when its workspace is deleted.
struct OciRepositoryRoute {
    std::string customer_id, name, workspace_id, created_at, updated_at;
};
// The volume snapshot policy of one target (migration 0069, operator#260, decision 54 S13):
// `store` (lake's own store volume) or a workspace id. Content-free: a five-field cron schedule
// in UTC, how many snapshots the operator keeps, and `requested_at`, a one-off request the
// operator honors when it is newer than the target's last snapshot. `status` is the operator's
// observation (volume_snapshot_status): its last VolumeSnapshot's name, whether it is readyToUse,
// when it was taken, and why it is not; empty fields until the operator has written one.
struct VolumeSnapshotStatus {
    std::string snapshot_name;
    bool ready_to_use = false;
    std::string snapshot_at, message, updated_at;
};
struct VolumeSnapshotPolicy {
    std::string target, schedule = "0 3 * * *";
    std::int32_t keep = 7;
    std::string requested_at, created_at, updated_at;
    std::optional<VolumeSnapshotStatus> status;
};
// request_volume_snapshot's input: the target and, optionally, a new schedule and keep.
struct VolumeSnapshotRequest {
    std::string target;
    std::optional<std::string> schedule;
    std::optional<std::int32_t> keep;
};
inline constexpr std::int32_t max_workspace_storage_gi = 32767;
inline constexpr std::int32_t max_volume_snapshots_kept = 100;
// A five-field cron expression in UTC, as the operator parses it (robfig/cron's standard parser):
// minute 0-59, hour 0-23, day of month 1-31, month 1-12, day of week 0-6; each field a comma list
// of `*`, `n`, `a-b`, each optionally `/step`. It is refused when it can never fire: the day of week
// is unrestricted (`*...`), so the day of month must match, and no selected month has any selected
// day (February counts 29 days). Mirrors the operator's check, whose schedule.Next is then zero.
namespace detail {
inline bool parse_cron_field(std::string_view field, int low, int high, std::uint64_t& bits) {
    if (field.empty()) {
        return false;
    }
    const auto number = [](std::string_view text, int& out) {
        if (text.empty() || text.size() > 2) {
            return false;
        }
        out = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
            out = out * 10 + (c - '0');
        }
        return true;
    };
    while (!field.empty()) {
        const std::size_t comma = field.find(',');
        std::string_view part = field.substr(0, comma);
        field = comma == std::string_view::npos ? std::string_view() : field.substr(comma + 1);
        if (comma != std::string_view::npos && field.empty()) {
            return false;
        }
        int step = 1;
        if (const std::size_t slash = part.find('/'); slash != std::string_view::npos) {
            if (!number(part.substr(slash + 1), step) || step < 1) {
                return false;
            }
            part = part.substr(0, slash);
        }
        int first = low;
        int last = high;
        if (part != "*") {
            const std::size_t dash = part.find('-');
            if (dash == std::string_view::npos) {
                if (!number(part, first)) {
                    return false;
                }
                last = step > 1 ? high : first;
            } else if (!number(part.substr(0, dash), first) || !number(part.substr(dash + 1), last)) {
                return false;
            }
        }
        if (first < low || last > high || first > last) {
            return false;
        }
        for (int value = first; value <= last; value += step) {
            bits |= std::uint64_t{1} << value;
        }
    }
    return true;
}
} // namespace detail

inline bool valid_snapshot_schedule(std::string_view schedule) {
    std::array<std::string_view, 5> fields;
    std::size_t count = 0;
    while (!schedule.empty()) {
        const std::size_t space = schedule.find(' ');
        const std::string_view field = schedule.substr(0, space);
        if (field.empty() || count == fields.size()) {
            return false;
        }
        fields[count++] = field;
        if (space == std::string_view::npos) {
            break;
        }
        schedule.remove_prefix(space + 1);
        if (schedule.empty()) {
            return false;
        }
    }
    if (count != fields.size()) {
        return false;
    }
    constexpr std::array<std::pair<int, int>, 5> ranges{{{0, 59}, {0, 23}, {1, 31}, {1, 12}, {0, 6}}};
    std::array<std::uint64_t, 5> bits{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!detail::parse_cron_field(fields[i], ranges[i].first, ranges[i].second, bits[i])) {
            return false;
        }
    }
    if (fields[2].starts_with('*') || !fields[4].starts_with('*')) {
        return true; // every day of the month, or the day of week can match
    }
    constexpr std::array<int, 13> days_in_month{0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int month = 1; month <= 12; ++month) {
        if ((bits[3] >> month & 1U) == 0) {
            continue;
        }
        for (int day = 1; day <= days_in_month[month]; ++day) {
            if ((bits[2] >> day & 1U) != 0) {
                return true;
            }
        }
    }
    return false;
}
// A tenant repository name below its space: `<image>` or `cache/<image>`, where `<image>` is one
// component of lowercase letters, digits, `.`, `_` and `-`, starting and ending with a letter or
// digit (OCI's repository grammar), at most 190 characters.
inline bool valid_oci_route_name(std::string_view name) {
    if (name.starts_with("cache/")) {
        name.remove_prefix(6);
    }
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return !name.empty() && name.size() <= 190 && alnum(name.front()) && alnum(name.back()) &&
           std::ranges::all_of(name, [&](char c) {
               return alnum(c) || c == '.' || c == '_' || c == '-';
           });
}
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
    // The stores the service mounts (migration 0080, lake#666), written in the transaction that
    // creates it. The caller has checked them (api/store_mounts.hpp); the store refuses a volume
    // another workload mounts.
    std::vector<StoreMountRequest> stores;
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
    // Where the store's contents are (lake#664, in `stores.spec`): for the customer's own bucket
    // (r2, s3, b2, gcs, azure) its name, endpoint and the folder in it the store covers, and the
    // workspace secret that opens it (decision 58: the workspace's credential, never Orama's);
    // for a `lake` store the liquid repository that keeps it. Empty otherwise.
    std::string bucket, endpoint_url, prefix;
    std::string credential_mount_id, credential_name;
    std::string repository_id;
    // The bytes in use and when the operator measured them (migration 0080, lake#666): empty until
    // it has mounted and measured the store.
    std::optional<std::int64_t> used_bytes;
    std::string used_bytes_at;
    // The services and apps that mount it (store_mounts), filled by list_stores and get_store.
    std::vector<StoreMount> mounted_by;
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
    // When the operator last wrote the launch's status: a finished job's end (lake#643).
    std::string status_updated_at;
};
struct NewAgentLaunch {
    std::string customer_id, agent_id, agent_name, mode, trigger;
    nlohmann::json input = nullptr;
    std::vector<std::string> scopes;
    std::string created_by_user_id, created_by_taap_id;
    std::string workspace_id;
};
// One run of a workspace's own methodology (docs/api/methodologies.md#runs, lake#655, migration
// 0078): lake writes the desired run, the Orama operator waits for its image build and runs its
// stage, run and collect pods, and writes the status. The stage and collect pods authenticate with
// `lmr.<run id>.<secret>`, which the operator generates; lake keeps only its SHA-256. Ids, images,
// resources and phases only: what the run read and produced is its record in the workspace Zetta.
struct MethodologyRun {
    std::string id, customer_id, workspace_id, methodology_id, mode, build_id, image, runtime_image,
        slug, desired;
    std::int64_t cpu_millis = 0, memory_bytes = 0;
    int gpus = 0;
    std::string gpu_class;
    int timeout_seconds = 0;
    bool network = false;
    std::int64_t generation = 1;
    std::string stop_reason;
    std::string created_by_user_id, created_by_taap_id, created_at, updated_at;
    // Written by the operator; empty until it reports.
    std::string phase, message;
    std::optional<int> exit_code;
    std::string credential_sha256;
    std::int64_t observed_generation = 0;
    std::string started_at, finished_at, status_updated_at;
};
struct NewMethodologyRun {
    std::string customer_id, workspace_id, methodology_id, mode, build_id, image, runtime_image;
    std::int64_t cpu_millis = 0, memory_bytes = 0;
    int gpus = 0;
    std::string gpu_class;
    int timeout_seconds = 0;
    bool network = false;
    std::string created_by_user_id, created_by_taap_id;
};
// What the operator reports about a run (methodology_run_status). The in-memory store's tests and
// the operator contract test write it; lake never does in production.
struct MethodologyRunStatus {
    std::string phase, message;
    std::optional<int> exit_code;
    std::string credential_sha256;
    std::int64_t observed_generation = 0;
    std::string started_at, finished_at;
};
// A methodology run's credential (`lmr.<run id>.<secret>`, lake#655) is live while the run is
// present and not finished; it acts as no person, and may call only methodology_run_input,
// methodology_run_artifact (lake#690) and methodology_run_output for its own run (the registry
// checks the run).
inline constexpr std::string_view methodology_run_credential_prefix = "lmr.";
inline bool methodology_run_credential_live(const MethodologyRun& run) {
    return run.desired == "present" && !run.credential_sha256.empty() && run.phase != "Succeeded" &&
           run.phase != "Failed" && run.phase != "Stopped";
}
inline Principal methodology_run_principal(const MethodologyRun& run, std::string digest) {
    return Principal{"",
                     "methodology_run:" + run.id,
                     "methodology_run:" + run.id,
                     "",
                     "lake",
                     "methodology_run",
                     {"methodology_run_input", "methodology_run_artifact", "methodology_run_output"},
                     std::nullopt,
                     run.id,
                     run.customer_id,
                     "",
                     "",
                     std::move(digest)};
}
// A workspace methodology's model served from Ship → Models (lake#690, migration 0083): lake writes
// the desired deployment when the model is published, the Orama operator stages the model's
// artifact with the deployment's credential and runs the methodology's image in `serve` mode as a
// Deployment behind a Service, and writes the status with the Service's in-cluster endpoint. The
// stage Job authenticates with `lmd.<deployment id>.<secret>`; lake keeps only its SHA-256. Ids,
// images, resources and phases only: the model and every prediction stay in the workspace Zetta.
struct MethodologyDeployment {
    std::string id, customer_id, workspace_id, methodology_id, model_id, run_id, build_id, image,
        runtime_image, slug, desired;
    std::int64_t cpu_millis = 0, memory_bytes = 0;
    int gpus = 0;
    std::string gpu_class;
    bool network = false;
    std::int64_t generation = 1;
    std::string stop_reason;
    std::string created_by_user_id, created_by_taap_id, created_at, updated_at;
    // Written by the operator; empty until it reports.
    std::string phase, message, credential_sha256, endpoint;
    std::int64_t observed_generation = 0;
    std::string ready_at, stopped_at, status_updated_at;
};
struct NewMethodologyDeployment {
    std::string customer_id, workspace_id, methodology_id, model_id, run_id, build_id, image,
        runtime_image;
    std::int64_t cpu_millis = 0, memory_bytes = 0;
    int gpus = 0;
    std::string gpu_class;
    bool network = false;
    std::string created_by_user_id, created_by_taap_id;
};
// What the operator reports about a deployment (methodology_deployment_status). Tests and the
// operator contract write it; lake never does in production.
struct MethodologyDeploymentStatus {
    std::string phase, message, credential_sha256, endpoint;
    std::int64_t observed_generation = 0;
    std::string ready_at, stopped_at;
};
// A deployment's credential (`lmd.<deployment id>.<secret>`, lake#690) is live while the deployment
// is present: the stage Job reads the model through methodology_deployment_input, and kubelet pulls
// the methodology's image with it whenever a serve pod (re)starts. It acts as no person.
inline constexpr std::string_view methodology_deployment_credential_prefix = "lmd.";
inline bool methodology_deployment_credential_live(const MethodologyDeployment& deployment) {
    return deployment.desired == "present" && !deployment.credential_sha256.empty() &&
           deployment.phase != "Stopped";
}
inline Principal methodology_deployment_principal(const MethodologyDeployment& deployment,
                                                  std::string digest) {
    return Principal{"",
                     "methodology_deployment:" + deployment.id,
                     "methodology_deployment:" + deployment.id,
                     "",
                     "lake",
                     "methodology_deployment",
                     {"methodology_deployment_input"},
                     std::nullopt,
                     deployment.id,
                     deployment.customer_id,
                     "",
                     "",
                     std::move(digest)};
}
// Whether `endpoint` is a serve Service the operator could have written: only
// `http://md-<12 hex>.<namespace>.svc.cluster.local[:port]`, so a predict is never sent elsewhere.
bool methodology_deployment_endpoint_valid(std::string_view endpoint);
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

// One Opportunity Auto-Suggest Agent recommended (or declined, status not_yet), kept across scans. It
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
struct NewManagedStore {
    std::string customer_id, type, provider, name, created_by_taap_id;
    int size_gi = 10; // a volume's; 0 for every other type, which has no size to ask for
    std::string region;
    bool is_public = false;
    std::string workspace_id; // required by the routing trigger (migration 0044)
    // As ManagedStore's (lake#664).
    std::string bucket, endpoint_url, prefix;
    std::string credential_mount_id, credential_name;
    std::string repository_id;
};
// `stores.spec` for a new store (docs/control-plane/storage.md): a volume's {size_gi, region,
// public}; a bucket's {public, bucket, endpoint, region, prefix, credential: {mount_id, name}}; a
// `lake` store's {public, repository_id}; otherwise {public}. Content-free: a secret is named,
// never copied.
inline nlohmann::json store_spec(const NewManagedStore& in) {
    nlohmann::json spec{{"public", in.is_public}};
    if (in.type == "volume") {
        spec["size_gi"] = in.size_gi;
        spec["region"] = in.region;
    }
    if (!in.bucket.empty()) {
        spec["bucket"] = in.bucket;
        spec["endpoint"] = in.endpoint_url;
        spec["region"] = in.region;
        spec["prefix"] = in.prefix;
        spec["credential"] = {{"mount_id", in.credential_mount_id}, {"name", in.credential_name}};
    }
    if (!in.repository_id.empty()) {
        spec["repository_id"] = in.repository_id;
    }
    return spec;
}
// One valid type × provider pair and whether this lake offers it. Order: what is available first,
// then by type. The schema's store_type_check and store_provider_check list the same keys.
struct StoreProvider {
    std::string_view type, provider, label;
    bool available = false;
    std::string_view unavailable_reason; // empty when available
};
inline constexpr StoreProvider store_providers[] = {
    {"volume", "cluster", "Volume — cluster disk", true, ""},
    // The customer's own bucket, opened with a secret in the workspace's secret store (lake#664).
    {"object", "r2", "Bucket — your Cloudflare R2 bucket", true, ""},
    {"object", "s3", "Bucket — your AWS S3 bucket", true, ""},
    {"object", "b2", "Bucket — your Backblaze B2 bucket", true, ""},
    {"object", "gcs", "Bucket — your Google Cloud Storage bucket", true, ""},
    {"object", "azure", "Bucket — your Azure Blob container", true, ""},
    // Kept by Lake in the workspace (lake#664).
    {"object", "zetta", "Bucket — kept by Lake in the workspace", true, ""},
    {"object", "lake", "Bucket — a liquid repository", true, ""},
    {"filesystem", "zetta", "Filesystem — kept by Lake in the workspace", true, ""},
    {"filesystem", "lake", "Filesystem — Drive (a liquid repository)", true, ""},
    {"network_filesystem", "nfs", "Network filesystem — NFS in the cluster", false, "phase 4"},
    {"network_filesystem", "azure", "Network filesystem — Azure Files", false, "phase 4"},
    {"network_filesystem", "s3", "Network filesystem — EFS", false, "phase 4"},
    {"network_filesystem", "gcs", "Network filesystem — Filestore", false, "phase 4"},
};
// The customer's own bucket at a cloud provider, reached over the provider's protocol with the
// workspace's credential (lake#664).
inline bool store_external_bucket(std::string_view type, std::string_view provider) {
    return type == "object" && (provider == "r2" || provider == "s3" || provider == "b2" ||
                                provider == "gcs" || provider == "azure");
}
// A store whose contents Lake keeps itself in the workspace (lake#664): the workspace Zetta or a
// liquid repository.
inline bool store_kept_by_lake(std::string_view type, std::string_view provider) {
    return (type == "object" || type == "filesystem") && (provider == "zetta" || provider == "lake");
}
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

nlohmann::json to_json(const User&);
nlohmann::json to_json(const Organization&);
nlohmann::json to_json(const Customer&);
nlohmann::json to_json(const Repository&);
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
    // See OrganizationDeletion. `conflict` while it owns a live workspace (the answer still names
    // them through `live_workspaces` on a dry run); `not_found` for an unknown or deleted one.
    virtual Result<OrganizationDeletion> delete_organization(const std::string& id,
                                                             bool dry_run) = 0;

    virtual Result<Customer> create_customer(const NewCustomer&) = 0;
    virtual Result<std::vector<Customer>> list_customers(int limit,
                                                         const std::string& user_id = {}) = 0;
    virtual Result<Customer> get_customer(const std::string& id) = 0;
    // The organization's own space; not_found when it has none (lake#561, get_organization).
    virtual Result<Customer> organization_customer(const std::string& organization_id) = 0;

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
    // Liquid repository ids (lake#805, docs/api/platform.md#which-repository-a-row-names). The
    // live repositories whose recorded liquid id is `liquid_id` (more than one when a repository
    // was pushed from another's clone); records one (`not_found` for an unknown repository); and
    // the live repositories with a workspace whose id lake has not read yet, oldest first.
    virtual Result<std::vector<Repository>> repositories_by_liquid_id(const std::string& liquid_id) = 0;
    virtual Result<void> set_repository_liquid_id(const std::string& id,
                                                  const std::string& liquid_id) = 0;
    virtual Result<std::vector<Repository>> repositories_without_liquid_id(int limit) = 0;
    // The central audit events about a workspace's data (decision 52, lake#317), for the
    // one-shot copy into workspace Zettas only. Assigning records the workspace of each event
    // whose action is one of `workspace_actions` and that was not refused for want of authority,
    // as migration 0063 does, for events an earlier lake wrote after it. Listing pages the
    // assigned events in append order after the event `after_id`.
    virtual Result<void>
    assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) = 0;
    virtual Result<std::vector<LegacyAuditEvent>> legacy_audit_events(const std::string& after_id,
                                                                      int limit) = 0;
    // Drive share links live in workspace Zettas (lake#350). The control store answers only the
    // route of a link made before the move, from its token digest; `not_found` otherwise.
    virtual Result<DriveShareLinkRoute> drive_share_link_route(const std::string& token_hash) = 0;

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
    // A pending invitation becomes `revoked` (lake#645) and can no longer be accepted; with
    // `dry_run` it is only checked. `conflict` for one that is accepted, expired or revoked.
    virtual Result<Invitation> revoke_invitation(const std::string& id, bool dry_run) = 0;
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
    virtual Result<Grant> get_grant(const std::string& id) = 0;
    // Removes one grant (lake#645), or with `dry_run` only checks that it could. `conflict` for a
    // grant a membership derives (remove_member removes it with the membership) and for the last
    // `admin` grant on a space, which would leave the space without an administrator.
    virtual Result<Grant> revoke_grant(const std::string& id, bool dry_run) = 0;
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

    virtual audit::Result<audit::Identity> current_lake_identity() = 0;
    virtual audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) = 0;
    virtual audit::Result<void> create_lake_identity(const audit::Identity& identity) = 0;
    virtual audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                                     const audit::Identity& replacement) = 0;

    virtual Result<void> append_audit_event(const audit::Event& event) = 0;
    virtual Result<audit::Event> latest_audit_event() = 0;
    virtual Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) = 0;
    virtual Result<void> set_audit_retention_days(const std::string& customer_id, int days) = 0;

    virtual Result<std::optional<std::uint64_t>> storage_quota(const std::string& customer_id) = 0;
    virtual Result<std::vector<Workspace>> list_workspaces(const std::string& customer_id) = 0;
    virtual Result<Workspace> get_workspace(const std::string& id) = 0;
    virtual Result<Workspace> create_workspace(const NewWorkspace&) = 0;
    virtual Result<Workspace> update_workspace(const std::string& id, const WorkspacePatch&) = 0;
    // Records that the workspace's repositories are now served from its own store in its workspace
    // Zetta (lake#380, move_workspace_store `switch`). Idempotent: a workspace already switched
    // keeps its first instant. A deleted workspace is a conflict.
    virtual Result<Workspace> mark_workspace_store_moved(const std::string& id) = 0;
    // Deletes a workspace and everything it holds in one transaction (lake#514): every repository
    // of the workspace still live or inside its retention is tombstoned with its retention ended
    // (`deleted_at` kept where set, `retention_until` now), which releases its name; its apps are
    // deleted and its unfinished builds cancelled; the workspace's desired state becomes
    // `deleted` with its services absent, so the operator deletes its Zetta; and the deletion is
    // recorded with what it destroyed. Repeating it for a workspace it deleted answers the
    // recorded deletion unchanged; a workspace deleted before migration 0065 is a conflict.
    virtual Result<WorkspaceDeletion> delete_workspace_cascade(const WorkspaceDeletionRequest&) = 0;
    // The recorded deletion of a workspace; not_found when it has none.
    virtual Result<WorkspaceDeletion> get_workspace_deletion(const std::string& workspace_id) = 0;
    // The deletions lake has not completed, oldest first.
    virtual Result<std::vector<WorkspaceDeletion>> pending_workspace_deletions(int limit) = 0;
    virtual Result<void> complete_workspace_deletion(const std::string& workspace_id) = 0;
    // Grows the workspace Zetta's volumes (migration 0069, operator#259): sets `storage_gi` and
    // advances the generation so the operator reconciles it. `invalid_input` when the workspace is
    // unknown or deleted, the size is outside 1..max_workspace_storage_gi, or it is below the
    // current size (volumes never shrink); the same size answers the row unchanged.
    virtual Result<Workspace> set_workspace_storage(const std::string& id, std::int32_t storage_gi) = 0;
    // Records a one-off snapshot request for a target (requested_at = now) and, when given, its new
    // schedule and keep, creating the target's policy with the defaults when it has none.
    // `invalid_input` when the target is neither `store` nor a known, live workspace id, or the
    // schedule or keep is invalid.
    virtual Result<VolumeSnapshotPolicy> request_volume_snapshot(const VolumeSnapshotRequest&) = 0;
    // Every snapshot policy with the operator's status, `store` first, then by target.
    virtual Result<std::vector<VolumeSnapshotPolicy>> volume_snapshots() = 0;
    // Tenant image routes (lake#383, docs/api/registry.md#where-a-tenant-image-lives). Creating a
    // route names its workspace explicitly: `invalid_input` when the name is invalid or the
    // workspace is not a live workspace of the space, the existing route when the same workspace
    // already holds the name, and `conflict` when another workspace does. A route is never
    // changed; delete_workspace_cascade deletes the workspace's routes in its transaction.
    virtual Result<OciRepositoryRoute> create_oci_repository_route(const std::string& customer_id,
                                                                   const std::string& name,
                                                                   const std::string& workspace_id) = 0;
    // `not_found` when the space has no route of that name.
    virtual Result<OciRepositoryRoute> get_oci_repository_route(const std::string& customer_id,
                                                                const std::string& name) = 0;
    // A space's routes by name; `workspace_id` empty: every workspace.
    virtual Result<std::vector<OciRepositoryRoute>>
    list_oci_repository_routes(const std::string& customer_id, const std::string& workspace_id) = 0;
    // Removes a route, or moves it (the operator's unroute_image_repository), in one transaction:
    // removing `<image>` also removes `cache/<image>`; with `to_workspace_id` each removed route is
    // written again for that workspace (`invalid_input` unless it is live in the space), so no
    // push in between can claim the name. The workspace's records stay where they are, and after a
    // plain removal a later create or copy may route the name again. Answers the routes removed,
    // `name`'s first; `not_found` when `name` has none.
    virtual Result<std::vector<OciRepositoryRoute>>
    delete_oci_repository_route(const std::string& customer_id, const std::string& name,
                                const std::string& to_workspace_id = {}) = 0;
    // Managed services (docs/control-plane/services.md). Lists the customer's services, oldest
    // first, leaving out deleted ones the operator has finished with.
    virtual Result<std::vector<ManagedService>> list_services(const std::string& customer_id) = 0;
    // Secure networking (docs/control-plane/network.md). Updates replace the mutable spec and
    // advance generation. Withdrawn/revoked/expired rows cannot be revived. Reads include history
    // and the operator's optional status; only the operator writes status in PostgreSQL.
    virtual Result<NetworkConnection> create_network_connection(const NetworkScope&,
                                                                const NetworkConnectionSpec&,
                                                                const NetworkActor&) = 0;
    virtual Result<NetworkConnection> update_network_connection(const NetworkScope&,
                                                                const std::string& id,
                                                                const NetworkConnectionSpec&,
                                                                const NetworkActor&) = 0;
    virtual Result<NetworkConnection> withdraw_network_connection(const NetworkScope&,
                                                                  const std::string& id,
                                                                  const NetworkActor&) = 0;
    virtual Result<NetworkConnection> get_network_connection(const NetworkScope&,
                                                             const std::string& id) = 0;
    virtual Result<std::vector<NetworkConnection>>
    list_network_connections(const NetworkScope&) = 0;
    virtual Result<NetworkEgressRule> create_network_egress_rule(const NetworkScope&,
                                                                 const NetworkEgressRuleSpec&,
                                                                 const NetworkActor&) = 0;
    virtual Result<NetworkEgressRule> update_network_egress_rule(const NetworkScope&,
                                                                 const std::string& id,
                                                                 const NetworkEgressRuleSpec&,
                                                                 const NetworkActor&) = 0;
    virtual Result<NetworkEgressRule> withdraw_network_egress_rule(const NetworkScope&,
                                                                   const std::string& id,
                                                                   const NetworkActor&) = 0;
    virtual Result<NetworkEgressRule> get_network_egress_rule(const NetworkScope&,
                                                              const std::string& id) = 0;
    virtual Result<std::vector<NetworkEgressRule>>
    list_network_egress_rules(const NetworkScope&) = 0;
    virtual Result<NetworkTunnelGrant> create_network_tunnel_grant(const NetworkScope&,
                                                                   const NetworkTunnelGrantSpec&,
                                                                   const NetworkActor&) = 0;
    virtual Result<NetworkTunnelGrant> update_network_tunnel_grant(const NetworkScope&,
                                                                   const std::string& id,
                                                                   const NetworkTunnelGrantSpec&,
                                                                   const NetworkActor&) = 0;
    virtual Result<NetworkTunnelGrant> revoke_network_tunnel_grant(const NetworkScope&,
                                                                   const std::string& id,
                                                                   const NetworkActor&) = 0;
    virtual Result<NetworkTunnelGrant> get_network_tunnel_grant(const NetworkScope&,
                                                                const std::string& id) = 0;
    virtual Result<std::vector<NetworkTunnelGrant>>
    list_network_tunnel_grants(const NetworkScope&) = 0;
    virtual Result<NetworkPeer>
    create_network_peer(const NetworkScope&, const NetworkPeerSpec&, const NetworkActor&) = 0;
    virtual Result<NetworkPeer> update_network_peer(const NetworkScope&,
                                                    const std::string& id,
                                                    const NetworkPeerSpec&,
                                                    const NetworkActor&) = 0;
    virtual Result<NetworkPeer>
    withdraw_network_peer(const NetworkScope&, const std::string& id, const NetworkActor&) = 0;
    virtual Result<NetworkPeer> get_network_peer(const NetworkScope&, const std::string& id) = 0;
    virtual Result<std::vector<NetworkPeer>> list_network_peers(const NetworkScope&) = 0;
    virtual Result<ManagedService> get_service(const std::string& id) = 0;
    // Creates a service. A default one that already exists is returned unchanged; a live name is a
    // conflict; a customer at max_per_customer live services is refused with quota_exceeded.
    virtual Result<ManagedService> create_service(const NewManagedService&) = 0;
    // Marks a service absent so the operator deletes it, and removes its store mounts. Refuses one
    // already absent.
    virtual Result<ManagedService> withdraw_service(const std::string& id) = 0;
    // Replaces the stores a live service mounts (lake#666): bumps its generation and NOTIFYs
    // lake_services in the same transaction. A store must be a present store of the service's
    // workspace; a volume another workload mounts is a conflict naming it.
    virtual Result<ManagedService> set_service_stores(const std::string& service_id,
                                                      const std::vector<StoreMountRequest>&) = 0;
    // Every store mount of the customer's stores, with the store's and the workload's names
    // (lake#666); an empty workspace_id lists every workspace's.
    virtual Result<std::vector<StoreMount>>
    list_store_mounts(const std::string& customer_id, const std::string& workspace_id = {}) = 0;
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
    // Marks a store absent so the operator deletes it and its data. Refuses one already absent,
    // and one a service or an app mounts (conflict, naming them: lake#666).
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
    // Methodology runs (lake#655). Creating or withdrawing one NOTIFYs lake_services.
    virtual Result<MethodologyRun> create_methodology_run(const NewMethodologyRun&) = 0;
    virtual Result<MethodologyRun> get_methodology_run(const std::string& id) = 0;
    // Newest first; an empty methodology_id lists every methodology's runs in the workspace.
    virtual Result<std::vector<MethodologyRun>>
    list_methodology_runs(const std::string& customer_id,
                          const std::string& workspace_id,
                          const std::string& methodology_id,
                          int limit) = 0;
    // Marks a run absent so the operator stops and removes its pods, recording `reason` (empty:
    // stopped by a person). Refuses one already absent.
    virtual Result<MethodologyRun> withdraw_methodology_run(const std::string& id,
                                                            const std::string& reason) = 0;
    // Withdraws every present run the operator has not reported on within `seconds` of its
    // creation, recording `reason`. Returns how many it withdrew.
    virtual Result<std::size_t> expire_unstarted_methodology_runs(std::int64_t seconds,
                                                                  const std::string& reason) = 0;
    // The operator's report, for tests and the operator contract; lake never calls it otherwise.
    virtual Result<void> put_methodology_run_status(const std::string& id,
                                                    const MethodologyRunStatus& status) = 0;
    // Methodology deployments (lake#690). Creating or withdrawing one NOTIFYs lake_services.
    // Creating refuses a second present deployment of the same model in a workspace (`conflict`).
    virtual Result<MethodologyDeployment>
    create_methodology_deployment(const NewMethodologyDeployment&) = 0;
    virtual Result<MethodologyDeployment> get_methodology_deployment(const std::string& id) = 0;
    // Newest first; an empty model_id lists every model's deployments in the workspace, absent
    // ones included.
    virtual Result<std::vector<MethodologyDeployment>>
    list_methodology_deployments(const std::string& customer_id,
                                 const std::string& workspace_id,
                                 const std::string& model_id,
                                 int limit) = 0;
    // Marks a deployment absent so the operator removes its pods, recording `reason`. Refuses one
    // already absent.
    virtual Result<MethodologyDeployment>
    withdraw_methodology_deployment(const std::string& id, const std::string& reason) = 0;
    // The operator's report, for tests and the operator contract; lake never calls it otherwise.
    virtual Result<void>
    put_methodology_deployment_status(const std::string& id,
                                      const MethodologyDeploymentStatus& status) = 0;
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
    // The tasks one person's conversation started in a workspace (kind ask or factory, any
    // agent), and every task those asked for in turn (parent_task_id, lake#451), oldest first:
    // what deleting the conversation removes (lake#673). Empty for an id that is not a UUID.
    virtual Result<std::vector<AgentTask>>
    list_conversation_agent_tasks(const std::string& customer_id,
                                  const std::string& workspace_id,
                                  const std::string& conversation_id,
                                  const std::string& requested_by_user_id) = 0;
    // Removes these tasks' dispatch rows (their status goes with them) when they belong to
    // `customer_id`; answers how many went. Ids of another space, or already gone, are skipped.
    virtual Result<std::size_t> delete_agent_tasks(const std::string& customer_id,
                                                   const std::vector<std::string>& ids) = 0;
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
    virtual Result<std::uint64_t> set_storage_quota(const std::string& customer_id,
                                                    std::uint64_t quota_bytes) = 0;
    // The customer's usage limits (migration 0072, lake#564), by dimension; not_found for an
    // unknown customer. set_usage_limit replaces one; a `limit` of 0 removes it.
    virtual Result<std::vector<UsageLimit>> usage_limits(const std::string& customer_id) = 0;
    virtual Result<UsageLimit> set_usage_limit(const UsageLimit& limit) = 0;
    // Billing records (migration 0073, lake#564, docs/api/billing.md). billing_plans lists every
    // stored price list publication, by position then id; put_billing_plan creates or replaces one
    // with its prices (lake#635: billing_capabilities.cpp writes one row per publication).
    // create_invoice stores an invoice and its lines, numbering it (conflict when the
    // account already has an invoice for the period that is not void); set_invoice_state moves a
    // due invoice to paid or void (conflict from any other state).
    virtual Result<std::vector<BillingPlan>> billing_plans() = 0;
    // The resources each workload is scheduled with (migration 0075, lake#643), every row. The
    // Orama operator writes them; put_workload_resources is the same write, for tests and local
    // demos (it replaces the row of the kind and id).
    virtual Result<std::vector<WorkloadResources>> workload_resources() = 0;
    virtual Result<void> put_workload_resources(const WorkloadResources& row) = 0;
    // MCP connection dispatch (migration 0079, lake#684, lake#685). put replaces the connection's
    // row, or removes it when it neither streams nor re-exports; remove removes it.
    // mcp_stream_dispatch lists the rows that stream, by workspace, at most `limit`;
    // mcp_exported_dispatch the rows of one workspace that re-export tools.
    virtual Result<void> put_mcp_connection_dispatch(const McpConnectionDispatch& row) = 0;
    virtual Result<void> remove_mcp_connection_dispatch(const std::string& workspace_id,
                                                        const std::string& connection_id) = 0;
    virtual Result<std::vector<McpConnectionDispatch>> mcp_stream_dispatch(int limit) = 0;
    virtual Result<std::vector<McpConnectionDispatch>>
    mcp_exported_dispatch(const std::string& workspace_id) = 0;
    // Each app's traffic through the ingress by hour (migration 0081, lake#661), the rows of
    // `since_hour` and later, oldest hour first. The Orama operator writes them;
    // add_app_transfer is the same write (it adds to the row of the app and hour), for tests and
    // local demos.
    virtual Result<std::vector<AppTransfer>> app_transfer(const std::string& since_hour) = 0;
    virtual Result<void> add_app_transfer(const AppTransfer& row) = 0;
    virtual Result<BillingPlan> put_billing_plan(const BillingPlan& plan) = 0;
    virtual Result<Invoice> create_invoice(const Invoice& invoice) = 0;
    virtual Result<std::vector<Invoice>> list_invoices(const std::string& customer_id) = 0;
    virtual Result<Invoice> get_invoice(const std::string& id) = 0;
    virtual Result<Invoice> set_invoice_state(const std::string& id,
                                              const std::string& state,
                                              const std::string& provider_reference,
                                              const std::string& note) = 0;
    // Payment providers (migration 0074, lake#620): the provider's id for an account
    // (billing_accounts; "" when it has none, not_found for an unknown customer), kept once:
    // set_billing_provider_customer answers the id already kept when there is one. A payment event
    // is applied once per (provider, event_id) in one transaction: recorded, then the invoice of
    // that provider with `reference` moves only while it is due.
    virtual Result<std::string> billing_provider_customer(const std::string& customer_id,
                                                          const std::string& provider) = 0;
    virtual Result<std::string>
    set_billing_provider_customer(const std::string& customer_id,
                                  const std::string& provider,
                                  const std::string& provider_customer_id) = 0;
    virtual Result<PaymentEventResult> apply_payment_event(const PaymentEvent& event) = 0;
    // Cloud orders (lake#831, migration 0089, docs/api/orders.md).
    // Records a new order and its first event (actor lake).
    virtual Result<CloudOrder> create_cloud_order(const NewCloudOrder& order) = 0;
    virtual Result<CloudOrder> get_cloud_order(const std::string& id) = 0;
    virtual Result<CloudOrder> transition_cloud_order(const CloudOrderTransition& change) = 0;
    virtual Result<CloudOrderPaymentResult>
    apply_cloud_order_payment(const CloudOrderPayment& payment) = 0;
    // Orders with an email due (a recipient not yet told about the order's state), at most `limit`,
    // each held for `lease_seconds` so another pass does not send it twice.
    virtual Result<std::vector<CloudOrder>> claim_cloud_order_emails(int limit,
                                                                     int lease_seconds) = 0;
    virtual Result<void> record_cloud_order_email(const CloudOrderEmailOutcome& outcome) = 0;
    // Events not yet in the central audit log, oldest first, and marking them recorded.
    virtual Result<std::vector<CloudOrderEvent>> unaudited_cloud_order_events(int limit) = 0;
    virtual Result<void> mark_cloud_order_events_audited(const std::vector<std::int64_t>& ids) = 0;

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
    // The last step of deleting a mount (lake#645), after its records left its workspace: its
    // route, its leases' routes and its policy object (with its secrets' objects and every grant
    // on them). `not_found` when no route names it.
    virtual Result<void> delete_secret_mount_route(const std::string& mount_id) = 0;
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
    // Whether the user has an active TOTP enrollment in the pool (the web app's `viewer`,
    // lake#561).
    virtual Result<bool> mfa_enrolled(const std::string& user_id, const std::string& pool_id) = 0;
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
    // Clerk webhooks (lake#765, docs/api/clerk.md#webhooks).
    // user.created: what a first sign_in_clerk does to the account, without a session. Repeating
    // it finds the same account.
    virtual Result<ClerkUserProvisioned> provision_clerk_user(const ClerkIdentity&) = 0;
    // user.updated: the email (when non-empty) and display name of the account linked to the
    // subject; not_found when none is linked, conflict when another account has the email.
    virtual Result<std::string> update_clerk_user(const ClerkIdentity&) = 0;
    // user.deleted: disables the account linked to the subject and revokes every session, token
    // and device of it. Nothing is erased. Answers the account's user id.
    virtual Result<std::string> disable_clerk_user(const std::string& subject) = 0;
    // Whether a webhook delivery (its svix-id) was already applied, and recording that it was.
    virtual Result<bool> clerk_webhook_applied(const std::string& delivery_id) = 0;
    virtual Result<void> record_clerk_webhook(const std::string& delivery_id,
                                              const std::string& event_type,
                                              const std::string& subject) = 0;
    // Using your own identity (lake#762, docs/api/identity.md#your-own-identity).
    // Which key the account signs with.
    virtual Result<AccountIdentity> account_identity(const std::string& user_id) = 0;
    // The managed key with its wrapped seed; not_found once it is destroyed or if there was none.
    virtual Result<ManagedKey> managed_key(const std::string& user_id) = 0;
    // Records a pending link. conflict when the own identity already belongs to an account.
    virtual Result<void> create_identity_link_challenge(const IdentityLinkChallenge&) = 0;
    // Consumes the user's unexpired, unused challenge once.
    virtual Result<IdentityLinkChallenge>
    consume_identity_link_challenge(const std::string& user_id, const std::string& challenge) = 0;
    // In one transaction: records the link and destroys the managed seed. conflict when the
    // account already uses its own identity or the own identity belongs to another account.
    virtual Result<AccountIdentity> link_own_identity(const IdentityLink&) = 0;
    // The account identifier a TAAP identity acts for: itself for an account's own identifier,
    // the account's identifier for a linked own identity; not_found otherwise.
    virtual Result<std::string> account_for_identity(const std::string& taap_id) = 0;
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
