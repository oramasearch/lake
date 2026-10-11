#include "lake/control/memory_store.hpp"
#include "lake/control/platform_records.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <random>
#include <set>

#include "lake/control/agent_runs.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/metering.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/stream_owners.hpp"
#include "secrets_crypto.hpp"

namespace lake::control {

std::string new_uuid_v4() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned> byte(0, 255);
    unsigned char bytes[16];
    for (auto& b : bytes)
        b = static_cast<unsigned char>(byte(rng));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80); // RFC 4122 variant
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

std::string iso8601_at(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(value - seconds).count();
    return std::format("{:%FT%T}.{:06}Z", seconds, microseconds);
}

std::string now_iso8601() {
    return iso8601_at(std::chrono::system_clock::now());
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

nlohmann::json nullable(const std::string& s) {
    return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
}

bool valid_object_type(std::string_view type) {
    static const std::set<std::string_view> types = {
        "customer",
        "organization",
        "team",
        "member",
        "repository",
        "path",
        "environment",
        "secret_mount",
        "secret",
        "application",
    };
    return types.contains(type);
}

bool valid_policy_role(std::string_view role) {
    static const std::set<std::string_view> roles = {
        "read",
        "write",
        "admin",
        "deploy",
        "review",
        "share",
    };
    return roles.contains(role);
}

bool valid_membership_role(std::string_view role) {
    return role == "owner" || role == "admin" || role == "member";
}

std::string policy_role_for_membership(std::string_view role) {
    return role == "member" ? "read" : "admin";
}

} // namespace

nlohmann::json to_json(const User& u) {
    return {{"id", u.id},
            {"taap_id", nullable(u.taap_id)},
            {"email", nullable(u.email)},
            {"display_name", u.display_name},
            {"created_at", u.created_at},
            {"updated_at", u.updated_at}};
}
nlohmann::json to_json(const Organization& o) {
    return {{"id", o.id},
            {"taap_id", nullable(o.taap_id)},
            {"display_name", o.display_name},
            {"created_at", o.created_at},
            {"updated_at", o.updated_at}};
}
nlohmann::json to_json(const Customer& c) {
    return {{"id", c.id},
            {"customer_type", c.customer_type},
            {"user_id", nullable(c.user_id)},
            {"organization_id", nullable(c.organization_id)},
            {"name", c.name},
            {"created_at", c.created_at},
            {"updated_at", c.updated_at}};
}

nlohmann::json to_json(const Repository& r) {
    return {{"id", r.id},
            {"customer_id", r.customer_id},
            // null only for a repository migration 0051 recorded as needs_selection.
            {"workspace_id", nullable(r.workspace_id)},
            {"name", r.name},
            {"description", r.description},
            {"visibility", r.visibility},
            {"deleted_at", nullable(r.deleted_at)},
            {"retention_until", nullable(r.retention_until)},
            {"created_at", r.created_at},
            {"updated_at", r.updated_at}};
}

nlohmann::json to_json(const ForgeRecord& record) {
    return {{"id", record.id},
            {"repository_id", record.repository_id},
            {"kind", record.kind},
            {"value", record.value},
            {"created_at", record.created_at},
            {"updated_at", record.updated_at}};
}

nlohmann::json to_json(const DriveShareLink& link) {
    return {{"id", link.id},
            {"repository_id", link.repository_id},
            {"object_id", link.object_id},
            {"node_id", link.node_id},
            {"path", link.path},
            {"role", link.role},
            {"expires_at", link.expires_at},
            {"revoked_at", nullable(link.revoked_at)},
            {"created_by_taap_id", link.created_by_taap_id},
            {"created_at", link.created_at}};
}

nlohmann::json to_json(const Team& team) {
    return {{"id", team.id},
            {"organization_id", team.organization_id},
            {"name", team.name},
            {"display_name", team.display_name},
            {"created_at", team.created_at},
            {"updated_at", team.updated_at}};
}

nlohmann::json to_json(const Membership& membership) {
    return {{"id", membership.id},
            {"organization_id", membership.organization_id},
            {"team_id", nullable(membership.team_id)},
            {"principal_type", membership.principal_type},
            {"principal_id", membership.principal_id},
            {"principal_taap_id", membership.principal_taap_id},
            {"role", membership.role},
            {"attestation_digest", membership.attestation_digest},
            {"granted_by_taap_id", membership.granted_by_taap_id},
            {"created_at", membership.created_at},
            {"updated_at", membership.updated_at}};
}

nlohmann::json to_json(const Invitation& invitation) {
    return {{"id", invitation.id},
            {"organization_id", invitation.organization_id},
            {"team_id", nullable(invitation.team_id)},
            {"invitee_type", invitation.invitee_type},
            {"invitee", invitation.invitee},
            {"role", invitation.role},
            {"expires_at", invitation.expires_at},
            {"status", invitation.status},
            {"accepted_membership_id", nullable(invitation.accepted_membership_id)},
            {"created_by_taap_id", invitation.created_by_taap_id},
            {"created_at", invitation.created_at},
            {"updated_at", invitation.updated_at}};
}

nlohmann::json to_json(const PolicyObject& object) {
    return {{"id", object.id},
            {"object_type", object.object_type},
            {"external_id", object.external_id},
            {"parent_id", nullable(object.parent_id)},
            {"inherit", object.inherit},
            {"created_at", object.created_at},
            {"updated_at", object.updated_at}};
}

nlohmann::json to_json(const Grant& grant) {
    return {{"id", grant.id},
            {"principal_taap_id", grant.principal_taap_id},
            {"object_id", grant.object_id},
            {"role", grant.role},
            {"attestation_digest", grant.attestation_digest},
            {"granted_by_taap_id", grant.granted_by_taap_id},
            {"created_at", grant.created_at},
            {"updated_at", grant.updated_at}};
}

std::string secret_mount_environment(const SecretMountRoute& route) {
    return route.organization_id.empty()
               ? "secrets/" + route.customer_id
               : "secrets/" + route.customer_id + "/" + route.organization_id;
}

nlohmann::json to_json(const SecretMountRoute& route) {
    return {{"mount_id", route.mount_id},
            {"customer_id", route.customer_id},
            {"organization_id", nullable(route.organization_id)},
            {"workspace_id", route.workspace_id},
            {"created_at", route.created_at}};
}

nlohmann::json to_json(const SecretMount& mount) {
    // The owner: the customer (an account's mount, organization_id null) or an organization.
    return {{"id", mount.id},
            {"customer_id", mount.customer_id},
            {"owner_type", mount.organization_id.empty() ? "customer" : "organization"},
            {"organization_id", nullable(mount.organization_id)},
            {"workspace_id", mount.workspace_id},
            {"name", mount.name},
            {"description", mount.description},
            {"created_at", mount.created_at},
            {"updated_at", mount.updated_at}};
}

nlohmann::json to_json(const SecretVersion& version) {
    return {{"mount_id", version.mount_id},
            {"name", version.name},
            {"version", version.version},
            {"value", version.value},
            {"custom_metadata", version.custom_metadata},
            {"created_at", version.created_at},
            {"deleted_at", nullable(version.deleted_at)},
            {"destroyed_at", nullable(version.destroyed_at)}};
}

nlohmann::json to_json(const SecretMetadata& metadata) {
    nlohmann::json versions = nlohmann::json::array();
    for (const SecretVersionMetadata& version : metadata.versions) {
        versions.push_back({{"version", version.version},
                            {"created_at", version.created_at},
                            {"deleted_at", nullable(version.deleted_at)},
                            {"destroyed_at", nullable(version.destroyed_at)}});
    }
    return {{"mount_id", metadata.mount_id},
            {"name", metadata.name},
            {"current_version", metadata.current_version},
            {"custom_metadata", metadata.custom_metadata},
            {"versions", std::move(versions)},
            {"created_at", metadata.created_at},
            {"updated_at", metadata.updated_at}};
}

nlohmann::json to_json(const DynamicSecretLease& value) {
    return {{"id", value.id},
            {"mount_id", value.mount_id},
            {"workspace_id", value.workspace_id},
            {"role", value.role},
            {"expires_at", value.expires_at},
            {"revoked_at", nullable(value.revoked_at)},
            {"expired_at", nullable(value.expired_at)},
            {"renewals", value.renewals},
            {"created_at", value.created_at}};
}

nlohmann::json to_json(const EncryptionKey& key) {
    return {{"id", key.id},
            {"mount_id", key.mount_id},
            {"name", key.name},
            {"derived_from", nullable(key.derived_from)},
            {"current_version", key.current_version},
            {"created_at", key.created_at},
            {"updated_at", key.updated_at}};
}

nlohmann::json to_json(const TransitValue& value) {
    return {
        {"value", value.value}, {"key_version", value.key_version}, {"algorithm", value.algorithm}};
}

nlohmann::json to_json(const TransitVerification& value) {
    return {
        {"valid", value.valid}, {"key_version", value.key_version}, {"algorithm", value.algorithm}};
}

nlohmann::json to_json(const DerivedEncryptionKey& value) {
    nlohmann::json output = to_json(value.key);
    output["parent_version"] = value.parent_version;
    return output;
}

nlohmann::json to_json(const CustomerKekRotation& value) {
    return {{"customer_id", value.customer_id},
            {"version", value.version},
            {"data_keys_rewrapped", value.data_keys_rewrapped}};
}

Result<void> InMemoryStore::ping() {
    return {};
}

Result<User> InMemoryStore::create_user(const NewUser& in) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_) {
        if (!in.taap_id.empty() && u.taap_id == in.taap_id)
            return std::unexpected(StoreError{"conflict", "taap_id already exists"});
        if (!in.email.empty() && lower(u.email) == lower(in.email))
            return std::unexpected(StoreError{"conflict", "email already exists"});
    }
    const std::string now = now_iso8601();
    users_.push_back(User{new_uuid_v4(), in.taap_id, in.email, in.display_name, now, now});
    return users_.back();
}

Result<std::vector<User>> InMemoryStore::list_users(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<User> out(users_.rbegin(), users_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

audit::Result<audit::Identity> InMemoryStore::current_lake_identity() {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& identity : identities_) {
        if (identity.current) {
            return identity;
        }
    }
    return std::unexpected(audit::Error{"not_found", "no Lake identity exists"});
}

audit::Result<audit::Identity> InMemoryStore::lake_identity_by_taap_id(const std::string& taap_id) {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& identity : identities_) {
        if (identity.taap_id == taap_id) {
            return identity;
        }
    }
    return std::unexpected(audit::Error{"not_found", "no Lake identity for " + taap_id});
}

audit::Result<void> InMemoryStore::create_lake_identity(const audit::Identity& identity) {
    std::lock_guard lock(mutex_);
    for (const audit::Identity& existing : identities_) {
        if (existing.current || existing.taap_id == identity.taap_id) {
            return std::unexpected(audit::Error{"conflict", "a Lake identity already exists"});
        }
    }
    identities_.push_back(identity);
    return {};
}

audit::Result<void> InMemoryStore::rotate_lake_identity(const audit::Identity& previous,
                                                        const audit::Identity& replacement) {
    std::lock_guard lock(mutex_);
    for (audit::Identity& existing : identities_) {
        if (existing.id == previous.id && existing.current) {
            existing.current = false;
            identities_.push_back(replacement);
            return {};
        }
    }
    return std::unexpected(audit::Error{"conflict", "the Lake identity changed before rotation"});
}

Result<void> InMemoryStore::append_audit_event(const audit::Event& event) {
    std::lock_guard lock(mutex_);
    const std::string previous_digest = audit_events_.empty() ? "" : audit_events_.back().digest;
    if (event.previous_digest != previous_digest) {
        return std::unexpected(
            StoreError{"conflict", "audit event predecessor changed before append"});
    }
    audit_events_.push_back(event);
    return {};
}

Result<audit::Event> InMemoryStore::latest_audit_event() {
    std::lock_guard lock(mutex_);
    if (audit_events_.empty()) {
        return std::unexpected(StoreError{"not_found", "no audit events exist"});
    }
    return audit_events_.back();
}

Result<std::vector<audit::Event>> InMemoryStore::list_audit_events(const audit::Filter& filter) {
    std::lock_guard lock(mutex_);
    std::vector<audit::Event> events;
    bool after = filter.after_id.empty();
    for (const audit::Event& event : audit_events_) {
        if (!after) {
            after = event.id == filter.after_id;
            continue;
        }
        if (!filter.request_id.empty() && event.draft.request_id != filter.request_id) {
            continue;
        }
        if (!filter.principal.empty() && event.draft.principal != filter.principal) {
            continue;
        }
        if (!filter.object.empty() && event.draft.object != filter.object) {
            continue;
        }
        if (!filter.outcome.empty() && event.draft.outcome != filter.outcome) {
            continue;
        }
        if (!filter.from.empty() && event.occurred_at < filter.from) {
            continue;
        }
        if (!filter.to.empty() && event.occurred_at > filter.to) {
            continue;
        }
        if (event.draft.outcome == "success" &&
            std::ranges::find(filter.omit_successful_actions, event.draft.action) !=
                filter.omit_successful_actions.end()) {
            continue;
        }
        events.push_back(event);
    }
    if (filter.newest_first) {
        std::ranges::reverse(events);
    }
    if (filter.limit > 0 && events.size() > static_cast<std::size_t>(filter.limit)) {
        events.resize(static_cast<std::size_t>(filter.limit));
    }
    return events;
}

Result<void> InMemoryStore::set_audit_retention_days(const std::string& customer_id, int days) {
    std::lock_guard lock(mutex_);
    if (days < audit::retention_floor_days) {
        return std::unexpected(
            StoreError{"invalid_input", "audit retention cannot be below 400 days"});
    }
    if (customer_id.empty()) {
        return std::unexpected(StoreError{"invalid_input", "customer_id is required"});
    }
    for (const Customer& customer : customers_) {
        if (customer.id == customer_id) {
            for (auto& retention : audit_retentions_) {
                if (retention.first == customer_id) {
                    if (days < retention.second) {
                        return std::unexpected(
                            StoreError{"conflict", "audit retention cannot be reduced"});
                    }
                    retention.second = days;
                    return {};
                }
            }
            audit_retentions_.emplace_back(customer_id, days);
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
}

Result<std::optional<std::uint64_t>> InMemoryStore::storage_quota(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
    }
    const auto quota = storage_quotas_.find(customer_id);
    if (quota == storage_quotas_.end()) {
        return std::optional<std::uint64_t>{};
    }
    return quota->second;
}

Result<std::vector<Workspace>> InMemoryStore::list_workspaces(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<Workspace> rows;
    for (const Workspace& row : workspaces_) {
        if (row.customer_id == customer_id && !(row.desired == "deleted" && row.phase == "Deleted")) {
            rows.push_back(row);
        }
    }
    return rows;
}

Result<Workspace> InMemoryStore::get_workspace(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
    }
    return *row;
}

Result<Workspace> InMemoryStore::create_workspace(const NewWorkspace& input) {
    std::lock_guard lock(mutex_);
    if (input.name.empty() || input.name.size() > 128 || !valid_workspace_environment(input.environment)) {
        return std::unexpected(StoreError{"invalid_input", "name and DNS-label safe environment are required"});
    }
    if (std::ranges::none_of(customers_, [&](const Customer& row) {
            return row.id == input.customer_id;
        })) {
        return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
    }
    for (const Workspace& row : workspaces_) {
        if (row.customer_id == input.customer_id && row.name == input.name && row.desired != "deleted") {
            return std::unexpected(StoreError{"conflict", "a workspace with this name already exists"});
        }
    }
    Workspace row;
    row.id = new_uuid_v4();
    row.customer_id = input.customer_id;
    row.name = input.name;
    row.environment = input.environment;
    row.phase = "Pending";
    row.created_at = row.updated_at = now_iso8601();
    ManagedService service;
    service.id = new_uuid_v4();
    service.workspace_id = row.id;
    service.customer_id = row.customer_id;
    service.kind = "zetta";
    service.name = managed_service_slug("ws", row.id);
    service.slug = managed_service_slug("ws", row.id);
    service.desired = "present";
    service.created_by_taap_id = input.created_by_taap_id;
    service.created_at = service.updated_at = row.created_at;
    service.is_default = std::ranges::none_of(services_, [&](const ManagedService& existing) {
        return existing.customer_id == input.customer_id && existing.is_default;
    });
    row.zetta_service_id = service.id;
    services_.push_back(service);
    workspaces_.push_back(row);
    // As the workspace_snapshot_policy trigger does (migration 0069).
    VolumeSnapshotPolicy policy;
    policy.target = row.id;
    policy.created_at = policy.updated_at = row.created_at;
    volume_snapshot_policies_.emplace(row.id, std::move(policy));
    return row;
}

Result<Workspace> InMemoryStore::mark_workspace_store_moved(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
    }
    if (row->desired == "deleted") {
        return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
    }
    if (row->store_moved_at.empty()) {
        row->store_moved_at = now_iso8601();
    }
    return *row;
}

Result<Workspace> InMemoryStore::update_workspace(const std::string& id, const WorkspacePatch& patch) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
    }
    if ((patch.name && (patch.name->empty() || patch.name->size() > 128)) ||
        (patch.environment && !valid_workspace_environment(*patch.environment)) ||
        (patch.desired && *patch.desired != "running" && *patch.desired != "stopped" && *patch.desired != "deleted")) {
        return std::unexpected(StoreError{"invalid_input", "invalid name, environment or desired"});
    }
    if (row->desired == "deleted") {
        return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
    }
    const std::string name = patch.name.value_or(row->name);
    for (const Workspace& other : workspaces_) {
        if (other.id != id && other.customer_id == row->customer_id && other.name == name && other.desired != "deleted") {
            return std::unexpected(StoreError{"conflict", "a workspace with this name already exists"});
        }
    }
    const std::string environment = patch.environment.value_or(row->environment);
    const std::string desired = patch.desired.value_or(row->desired);
    if (name != row->name || environment != row->environment || desired != row->desired) {
        row->name = name;
        row->environment = environment;
        row->desired = desired;
        ++row->generation;
        row->updated_at = now_iso8601();
        for (ManagedService& service : services_) {
            if (service.workspace_id == id &&
                (desired == "deleted" || service.id == row->zetta_service_id) &&
                service.desired != (desired == "running" ? "present" : "absent")) {
                service.desired = desired == "running" ? "present" : "absent";
                ++service.generation;
                service.updated_at = row->updated_at;
            }
        }
    }
    return *row;
}

Result<WorkspaceDeletion> InMemoryStore::delete_workspace_cascade(
    const WorkspaceDeletionRequest& request) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, request.workspace_id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
    }
    if (row->desired == "deleted") {
        const auto recorded =
            std::ranges::find(workspace_deletions_, row->id, &WorkspaceDeletion::workspace_id);
        if (recorded == workspace_deletions_.end()) {
            return std::unexpected(StoreError{"conflict", "the workspace is deleted"});
        }
        return *recorded;
    }
    const std::string now = now_iso8601();
    WorkspaceDeletion deletion;
    deletion.workspace_id = row->id;
    deletion.customer_id = row->customer_id;
    deletion.workspace_name = row->name;
    deletion.requested_by = request.requested_by;
    deletion.requested_at = now;
    for (Repository& repository : repositories_) {
        if (repository.workspace_id != row->id) {
            continue;
        }
        const bool live = repository.deleted_at.empty();
        if (!live && repository.retention_until <= now) {
            continue;
        }
        deletion.repositories.push_back(
            {repository.id, repository.name, live ? "live" : "tombstoned"});
        if (live) {
            repository.deleted_at = now;
        }
        repository.retention_until = now;
        repository.updated_at = now;
    }
    for (WebApp& app : web_apps_) {
        if (app.workspace_id == row->id && app.desired != "deleted") {
            deletion.apps.push_back({app.id, app.name, app.desired});
            app.desired = "deleted";
            ++app.generation;
            app.updated_at = now;
        }
    }
    for (Build& build : builds_) {
        if (build.workspace_id == row->id && build.desired == "queued" &&
            build.phase != "Succeeded" && build.phase != "Failed" && build.phase != "Cancelled") {
            deletion.builds.push_back({build.id, build.target, build.phase});
            build.desired = "cancelled";
            build.updated_at = now;
        }
    }
    for (const std::string& image : request.images) {
        deletion.images.push_back({"", image, "tagged"});
    }
    // The workspace's tenant image routes (lake#383) go in the same change; their manifests and
    // tags go with its Zetta.
    const auto owner = std::ranges::find(customers_, row->customer_id, &Customer::id);
    for (auto route = oci_repository_routes_.begin(); route != oci_repository_routes_.end();) {
        if (route->workspace_id != row->id) {
            ++route;
            continue;
        }
        const std::string image =
            (owner == customers_.end() ? std::string() : owner->name) + "/" + route->name;
        if (std::ranges::find(request.images, image) == request.images.end()) {
            deletion.images.push_back({"", image, "routed"});
        }
        route = oci_repository_routes_.erase(route);
    }
    std::ranges::sort(deletion.images, {}, &WorkspaceDeletionItem::name);
    row->desired = "deleted";
    ++row->generation;
    row->updated_at = now;
    for (ManagedService& service : services_) {
        if (service.workspace_id == row->id && service.desired != "absent") {
            service.desired = "absent";
            ++service.generation;
            service.updated_at = now;
        }
    }
    workspace_deletions_.push_back(deletion);
    return deletion;
}

Result<OciRepositoryRoute> InMemoryStore::create_oci_repository_route(
    const std::string& customer_id, const std::string& name, const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    if (!valid_oci_route_name(name)) {
        return std::unexpected(StoreError{"invalid_input", "name is not a valid image repository name"});
    }
    const auto workspace = std::ranges::find(workspaces_, workspace_id, &Workspace::id);
    if (workspace == workspaces_.end() || workspace->customer_id != customer_id ||
        workspace->desired == "deleted") {
        return std::unexpected(
            StoreError{"invalid_input", "workspace_id must name a live workspace of this account"});
    }
    const auto existing = std::ranges::find_if(oci_repository_routes_, [&](const auto& route) {
        return route.customer_id == customer_id && route.name == name;
    });
    if (existing != oci_repository_routes_.end()) {
        if (existing->workspace_id != workspace_id) {
            return std::unexpected(StoreError{
                "conflict", "image repository " + name + " belongs to another workspace"});
        }
        return *existing;
    }
    const std::string now = now_iso8601();
    oci_repository_routes_.push_back({customer_id, name, workspace_id, now, now});
    return oci_repository_routes_.back();
}

Result<OciRepositoryRoute> InMemoryStore::get_oci_repository_route(const std::string& customer_id,
                                                                   const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(oci_repository_routes_, [&](const auto& route) {
        return route.customer_id == customer_id && route.name == name;
    });
    if (found == oci_repository_routes_.end()) {
        return std::unexpected(StoreError{"not_found", "no image repository " + name});
    }
    return *found;
}

Result<std::vector<OciRepositoryRoute>>
InMemoryStore::list_oci_repository_routes(const std::string& customer_id,
                                          const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<OciRepositoryRoute> routes;
    for (const OciRepositoryRoute& route : oci_repository_routes_) {
        if (route.customer_id == customer_id &&
            (workspace_id.empty() || route.workspace_id == workspace_id)) {
            routes.push_back(route);
        }
    }
    std::ranges::sort(routes, {}, &OciRepositoryRoute::name);
    return routes;
}

Result<std::vector<OciRepositoryRoute>>
InMemoryStore::delete_oci_repository_route(const std::string& customer_id, const std::string& name,
                                           const std::string& to_workspace_id) {
    std::lock_guard lock(mutex_);
    if (!to_workspace_id.empty()) {
        const auto workspace = std::ranges::find(workspaces_, to_workspace_id, &Workspace::id);
        if (workspace == workspaces_.end() || workspace->customer_id != customer_id ||
            workspace->desired == "deleted") {
            return std::unexpected(StoreError{
                "invalid_input", "workspace_id must name a live workspace of this account"});
        }
    }
    std::vector<std::string> names{name};
    if (!name.starts_with("cache/")) {
        names.push_back("cache/" + name);
    }
    std::vector<OciRepositoryRoute> removed;
    for (const std::string& each : names) {
        const auto found = std::ranges::find_if(oci_repository_routes_, [&](const auto& route) {
            return route.customer_id == customer_id && route.name == each;
        });
        if (found == oci_repository_routes_.end()) {
            if (each == name) {
                return std::unexpected(StoreError{"not_found", "no image repository " + name});
            }
            continue;
        }
        removed.push_back(*found);
        if (to_workspace_id.empty()) {
            oci_repository_routes_.erase(found);
        } else {
            const std::string now = now_iso8601();
            *found = {customer_id, each, to_workspace_id, now, now};
        }
    }
    return removed;
}

Result<WorkspaceDeletion> InMemoryStore::get_workspace_deletion(const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(workspace_deletions_, workspace_id, &WorkspaceDeletion::workspace_id);
    if (found == workspace_deletions_.end()) {
        return std::unexpected(StoreError{"not_found", "no deletion of workspace " + workspace_id});
    }
    return *found;
}

Result<std::vector<WorkspaceDeletion>> InMemoryStore::pending_workspace_deletions(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<WorkspaceDeletion> pending;
    for (const WorkspaceDeletion& deletion : workspace_deletions_) {
        if (deletion.completed_at.empty() &&
            (limit <= 0 || pending.size() < static_cast<std::size_t>(limit))) {
            pending.push_back(deletion);
        }
    }
    return pending;
}

Result<void> InMemoryStore::complete_workspace_deletion(const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(workspace_deletions_, workspace_id, &WorkspaceDeletion::workspace_id);
    if (found == workspace_deletions_.end()) {
        return std::unexpected(StoreError{"not_found", "no deletion of workspace " + workspace_id});
    }
    if (found->completed_at.empty()) {
        found->completed_at = now_iso8601();
    }
    return {};
}

Result<Workspace> InMemoryStore::set_workspace_storage(const std::string& id, std::int32_t storage_gi) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end() || row->desired == "deleted") {
        return std::unexpected(StoreError{"invalid_input", "workspace_id is unknown"});
    }
    if (storage_gi < 1 || storage_gi > max_workspace_storage_gi) {
        return std::unexpected(StoreError{"invalid_input", "storage_gi is out of range"});
    }
    if (storage_gi < row->storage_gi) {
        return std::unexpected(StoreError{
            "invalid_input", "workspace storage never decreases: " + std::to_string(storage_gi) +
                                 " GiB is below " + std::to_string(row->storage_gi) + " GiB"});
    }
    if (storage_gi != row->storage_gi) {
        row->storage_gi = storage_gi;
        ++row->generation;
        row->updated_at = now_iso8601();
    }
    return *row;
}

Result<VolumeSnapshotPolicy> InMemoryStore::request_volume_snapshot(const VolumeSnapshotRequest& request) {
    std::lock_guard lock(mutex_);
    if (request.target != "store") {
        const auto row = std::ranges::find(workspaces_, request.target, &Workspace::id);
        if (row == workspaces_.end() || row->desired == "deleted") {
            return std::unexpected(StoreError{"invalid_input", "target is unknown"});
        }
    }
    if ((request.schedule && !valid_snapshot_schedule(*request.schedule)) ||
        (request.keep && (*request.keep < 1 || *request.keep > max_volume_snapshots_kept))) {
        return std::unexpected(StoreError{"invalid_input", "invalid schedule or keep"});
    }
    const std::string now = now_iso8601();
    auto [found, created] = volume_snapshot_policies_.try_emplace(request.target);
    VolumeSnapshotPolicy& policy = found->second;
    if (created) {
        policy.target = request.target;
        policy.created_at = now;
    }
    if (request.schedule) {
        policy.schedule = *request.schedule;
    }
    if (request.keep) {
        policy.keep = *request.keep;
    }
    policy.requested_at = policy.updated_at = now;
    VolumeSnapshotPolicy value = policy;
    if (const auto status = volume_snapshot_status_.find(request.target);
        status != volume_snapshot_status_.end()) {
        value.status = status->second;
    }
    return value;
}

Result<std::vector<VolumeSnapshotPolicy>> InMemoryStore::volume_snapshots() {
    std::lock_guard lock(mutex_);
    std::vector<VolumeSnapshotPolicy> values;
    if (const auto store = volume_snapshot_policies_.find("store");
        store != volume_snapshot_policies_.end()) {
        values.push_back(store->second);
    }
    for (const auto& [target, policy] : volume_snapshot_policies_) {
        if (target != "store") {
            values.push_back(policy);
        }
    }
    for (VolumeSnapshotPolicy& value : values) {
        if (const auto status = volume_snapshot_status_.find(value.target);
            status != volume_snapshot_status_.end()) {
            value.status = status->second;
        }
    }
    return values;
}

Result<void> InMemoryStore::set_workspace_storage_status(const std::string& id,
                                                         std::int32_t observed_gi,
                                                         std::string phase,
                                                         std::string message) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"not_found", "no workspace " + id});
    }
    row->storage_gi_observed = observed_gi;
    row->storage_phase = std::move(phase);
    row->storage_message = std::move(message);
    return {};
}

Result<void> InMemoryStore::set_volume_snapshot_status(const std::string& target,
                                                       VolumeSnapshotStatus status) {
    std::lock_guard lock(mutex_);
    status.updated_at = now_iso8601();
    volume_snapshot_status_[target] = std::move(status);
    return {};
}

Result<void> InMemoryStore::set_workspace_status(const std::string& id, std::string phase) {
    std::lock_guard lock(mutex_);
    const auto row = std::ranges::find(workspaces_, id, &Workspace::id);
    if (row == workspaces_.end()) {
        return std::unexpected(StoreError{"not_found", "no workspace " + id});
    }
    row->phase = std::move(phase);
    row->observed_generation = row->generation;
    return {};
}

Result<std::vector<ManagedService>> InMemoryStore::list_services(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<ManagedService> services;
    for (const ManagedService& service : services_) {
        if (service.customer_id != customer_id ||
            (service.desired == "absent" && service.phase == "Deleted")) {
            continue;
        }
        // A deleted workspace's primary Zetta is history once the workspace is Deleted.
        const auto owner = std::ranges::find(workspaces_, service.workspace_id, &Workspace::id);
        if (service.desired == "absent" && owner != workspaces_.end() &&
            owner->zetta_service_id == service.id && owner->desired == "deleted" &&
            owner->phase == "Deleted") {
            continue;
        }
        services.push_back(service);
    }
    return services;
}

Result<ManagedService> InMemoryStore::get_service(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const ManagedService& service : services_) {
        if (service.id == id) {
            return service;
        }
    }
    return std::unexpected(StoreError{"not_found", "no service " + id});
}

Result<ManagedService> InMemoryStore::create_service(const NewManagedService& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == in.customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    std::size_t live = 0;
    for (const ManagedService& service : services_) {
        if (service.customer_id != in.customer_id || service.desired != "present") {
            continue;
        }
        if (in.is_default && service.is_default) {
            return service;
        }
        if (service.name == in.name) {
            return std::unexpected(
                StoreError{"conflict", "a service named " + in.name + " already exists"});
        }
        ++live;
    }
    if (in.max_per_customer != 0 && live >= in.max_per_customer) {
        return std::unexpected(StoreError{"quota_exceeded",
                                          "this account already runs " +
                                              std::to_string(in.max_per_customer) + " services"});
    }
    ManagedService service;
    service.id = new_uuid_v4();
    service.customer_id = in.customer_id;
    service.workspace_id = in.workspace_id;
    service.kind = in.kind;
    service.name = in.name;
    service.slug = managed_service_slug("svc", service.id);
    if (in.is_default) {
        const std::string workspace = managed_service_slug("ws", in.customer_id);
        if (std::ranges::none_of(services_, [&](const ManagedService& existing) {
                return existing.slug == workspace;
            })) {
            service.slug = workspace;
        }
    }
    service.is_default = in.is_default;
    service.instances = in.instances;
    service.storage_gi = in.storage_gi;
    service.desired = "present";
    service.created_by_taap_id = in.created_by_taap_id;
    service.created_at = service.updated_at = now_iso8601();
    services_.push_back(service);
    return service;
}

Result<ManagedService> InMemoryStore::withdraw_service(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (ManagedService& service : services_) {
        if (service.id != id) {
            continue;
        }
        if (service.desired != "present") {
            return std::unexpected(StoreError{"conflict", "the service is already deleted"});
        }
        service.desired = "absent";
        ++service.generation;
        service.updated_at = now_iso8601();
        return service;
    }
    return std::unexpected(StoreError{"not_found", "no service " + id});
}

Result<void> InMemoryStore::set_service_status(const std::string& id,
                                               std::string phase,
                                               std::string message,
                                               std::string endpoint) {
    std::lock_guard lock(mutex_);
    for (ManagedService& service : services_) {
        if (service.id == id) {
            service.phase = std::move(phase);
            service.message = std::move(message);
            service.endpoint = std::move(endpoint);
            service.observed_generation = service.generation;
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "no service " + id});
}

Result<std::vector<WebApp>> InMemoryStore::list_web_apps(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<WebApp> apps;
    for (const WebApp& app : web_apps_) {
        if (app.customer_id == customer_id &&
            !(app.desired == "deleted" && app.phase == "Deleted")) {
            apps.push_back(app);
        }
    }
    return apps;
}

Result<WebApp> InMemoryStore::get_web_app(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find(web_apps_, id, &WebApp::id);
    if (app == web_apps_.end()) {
        return std::unexpected(StoreError{"not_found", "no app " + id});
    }
    return *app;
}

bool InMemoryStore::user_can_access_web_app_locked(const WebApp& app,
                                                   const std::string& user_id) const {
    const auto customer = std::ranges::find(customers_, app.customer_id, &Customer::id);
    if (customer == customers_.end()) {
        return false;
    }
    if (customer->customer_type == "individual") {
        return customer->user_id == user_id;
    }
    return std::ranges::any_of(memberships_, [&](const Membership& membership) {
        return membership.organization_id == customer->organization_id &&
               membership.principal_type == "user" && membership.principal_id == user_id;
    });
}

Result<WebApp> InMemoryStore::resolve_web_app_auth_host(const std::string& host,
                                                        const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find_if(web_apps_, [&](const WebApp& candidate) {
        return candidate.host == host && candidate.auth == "lake" &&
               candidate.desired != "deleted" &&
               (workspace_id.empty() || candidate.workspace_id == workspace_id);
    });
    if (app == web_apps_.end() ||
        std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == app->workspace_id && workspace.desired != "deleted";
        })) {
        return std::unexpected(StoreError{"not_found", "no protected app uses this host"});
    }
    return *app;
}

Result<WebApp> InMemoryStore::authorize_web_app(const std::string& app_id,
                                                const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find(web_apps_, app_id, &WebApp::id);
    if (app == web_apps_.end() || app->auth != "lake" || app->desired == "deleted" ||
        std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == app->workspace_id && workspace.desired != "deleted";
        })) {
        return std::unexpected(StoreError{"not_found", "no protected app " + app_id});
    }
    if (!user_can_access_web_app_locked(*app, user_id)) {
        return std::unexpected(StoreError{"unauthorized", "user is not a member of this account"});
    }
    return *app;
}

Result<WebApp> InMemoryStore::authorize_web_app_session(const std::string& host,
                                                        const std::string& workspace_id,
                                                        const std::string& app_id,
                                                        const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find_if(web_apps_, [&](const WebApp& candidate) {
        return candidate.id == app_id && candidate.host == host && candidate.auth == "lake" &&
               candidate.desired != "deleted" && candidate.workspace_id == workspace_id;
    });
    if (app == web_apps_.end() ||
        std::ranges::none_of(workspaces_,
                             [&](const Workspace& workspace) {
                                 return workspace.id == app->workspace_id &&
                                        workspace.desired != "deleted";
                             }) ||
        !user_can_access_web_app_locked(*app, user_id)) {
        return std::unexpected(StoreError{"unauthorized", "app session is not authorized"});
    }
    return *app;
}

Result<void>
InMemoryStore::create_web_app_authorization_code(const WebAppAuthorizationCode& value) {
    if (value.app_id.empty() || value.user_id.empty() || value.host.empty() ||
        value.code_hash.empty()) {
        return std::unexpected(StoreError{"invalid_input", "app authorization code is invalid"});
    }
    std::lock_guard lock(mutex_);
    std::erase_if(web_app_authorization_codes_, [](const auto& candidate) {
        return candidate.used || candidate.expires_at <= std::chrono::steady_clock::now();
    });
    if (std::ranges::any_of(web_app_authorization_codes_, [&](const auto& candidate) {
            return candidate.code.code_hash == value.code_hash;
        })) {
        return std::unexpected(StoreError{"conflict", "app authorization code already exists"});
    }
    web_app_authorization_codes_.push_back(
        {value, std::chrono::steady_clock::now() + std::chrono::seconds(60), false});
    return {};
}

Result<WebAppAuthorization> InMemoryStore::consume_web_app_authorization_code(
    const std::string& host, const std::string& workspace_id, const std::string& code_hash) {
    std::lock_guard lock(mutex_);
    const auto code =
        std::ranges::find_if(web_app_authorization_codes_, [&](const auto& candidate) {
            return candidate.code.host == host && candidate.code.code_hash == code_hash &&
                   !candidate.used && candidate.expires_at > std::chrono::steady_clock::now();
        });
    if (code == web_app_authorization_codes_.end()) {
        return std::unexpected(
            StoreError{"unauthorized", "app authorization code is invalid or expired"});
    }
    const auto app = std::ranges::find(web_apps_, code->code.app_id, &WebApp::id);
    if (app == web_apps_.end() || app->host != host || app->auth != "lake" ||
        app->desired == "deleted" || (!workspace_id.empty() && app->workspace_id != workspace_id) ||
        std::ranges::none_of(workspaces_,
                             [&](const Workspace& workspace) {
                                 return workspace.id == app->workspace_id &&
                                        workspace.desired != "deleted";
                             }) ||
        !user_can_access_web_app_locked(*app, code->code.user_id)) {
        return std::unexpected(
            StoreError{"unauthorized", "app authorization code is not authorized"});
    }
    code->used = true;
    return WebAppAuthorization{*app, code->code.user_id};
}

Result<void> InMemoryStore::sweep_web_app_authorization_codes() {
    std::lock_guard lock(mutex_);
    std::erase_if(web_app_authorization_codes_, [](const auto& candidate) {
        return candidate.used || candidate.expires_at <= std::chrono::steady_clock::now();
    });
    return {};
}

Result<WebApp> InMemoryStore::create_web_app(const NewWebApp& input) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == input.workspace_id && workspace.customer_id == input.customer_id &&
                   workspace.desired != "deleted";
        })) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
    }
    if (std::ranges::none_of(customers_, [&](const Customer& customer) {
            return customer.id == input.customer_id;
        })) {
        return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
    }
    std::size_t app_count = 0;
    std::size_t replica_count = 0;
    for (const WebApp& app : web_apps_) {
        if (app.desired == "deleted") {
            continue;
        }
        if (app.host == input.host) {
            return std::unexpected(
                StoreError{"conflict", "an app already uses host " + input.host});
        }
        if (app.customer_id != input.customer_id) {
            continue;
        }
        if (app.name == input.name) {
            return std::unexpected(
                StoreError{"conflict", "an app named " + input.name + " already exists"});
        }
        ++app_count;
        replica_count += static_cast<std::size_t>(app.replicas);
    }
    if (input.max_apps != 0 && app_count >= input.max_apps) {
        return std::unexpected(StoreError{"quota_exceeded", "app quota reached"});
    }
    if (input.max_replicas != 0 &&
        replica_count + static_cast<std::size_t>(input.replicas) > input.max_replicas) {
        return std::unexpected(StoreError{"quota_exceeded", "app replica quota reached"});
    }
    WebApp app;
    app.id = new_uuid_v4();
    app.customer_id = input.customer_id;
    app.workspace_id = input.workspace_id;
    app.name = input.name;
    app.mode = input.mode;
    app.source_repository = input.source_repository;
    app.source_revision = input.source_revision;
    app.source_path = input.source_path;
    app.source_entrypoint = input.source_entrypoint;
    app.image = input.image;
    app.framework = input.framework;
    app.port = input.port;
    app.replicas = input.replicas;
    app.host = input.host;
    app.auth = input.auth;
    app.bindings = input.bindings;
    app.env = input.env;
    app.secret_env_refs = input.secret_env_refs;
    app.updated_at = now_iso8601();

    web_apps_.push_back(app);
    return app;
}

Result<WebApp> InMemoryStore::update_web_app(const std::string& id, const WebAppPatch& patch) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(web_apps_, id, &WebApp::id);
    if (found == web_apps_.end()) {
        return std::unexpected(StoreError{"not_found", "no app " + id});
    }
    if (found->desired == "deleted") {
        return std::unexpected(StoreError{"conflict", "the app is already deleted"});
    }
    const int replicas = patch.replicas.value_or(found->replicas);
    const std::string desired = patch.desired.value_or(found->desired);
    std::size_t replica_count = 0;
    for (const WebApp& app : web_apps_) {
        if (app.customer_id != found->customer_id || app.id == id || app.desired == "deleted") {
            continue;
        }
        replica_count += static_cast<std::size_t>(app.replicas);
    }
    if (desired != "deleted") {
        replica_count += static_cast<std::size_t>(replicas);
    }
    if (patch.max_replicas != 0 && replica_count > patch.max_replicas) {
        return std::unexpected(StoreError{"quota_exceeded", "app replica quota reached"});
    }
    if (patch.mode) {
        found->mode = *patch.mode;
    }
    if (patch.source_repository) {
        found->source_repository = *patch.source_repository;
    }
    if (patch.source_revision) {
        found->source_revision = *patch.source_revision;
    }
    if (patch.source_path) {
        found->source_path = *patch.source_path;
    }
    if (patch.source_entrypoint) {
        found->source_entrypoint = *patch.source_entrypoint;
    }
    if (patch.rotate_tokens) {
        found->token_hashes = nlohmann::json::object();
    }
    if (patch.image) {
        found->image = *patch.image;
    }
    found->replicas = replicas;
    if (patch.auth) {
        found->auth = *patch.auth;
    }
    if (patch.bindings) {
        found->bindings = *patch.bindings;
    }
    if (patch.env) {
        found->env = *patch.env;
    }
    if (patch.secret_env_refs) {
        found->secret_env_refs = *patch.secret_env_refs;
    }
    found->desired = desired;
    ++found->generation;
    found->updated_at = now_iso8601();
    return *found;
}

Result<void> InMemoryStore::purge_deleted_web_apps() {
    std::lock_guard lock(mutex_);
    std::erase_if(web_apps_, [](const WebApp& app) {
        return app.desired == "deleted" && app.phase == "Deleted";
    });
    return {};
}

Result<void> InMemoryStore::set_web_app_token_hashes(const std::string& id, nlohmann::json hashes) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find(web_apps_, id, &WebApp::id);
    if (app == web_apps_.end()) {
        return std::unexpected(StoreError{"not_found", "no app " + id});
    }
    app->token_hashes = std::move(hashes);
    return {};
}

Result<void> InMemoryStore::set_web_app_status(const std::string& id,
                                               std::string phase,
                                               std::string url,
                                               bool ready,
                                               std::string message,
                                               std::int64_t observed_generation) {
    std::lock_guard lock(mutex_);
    const auto app = std::ranges::find(web_apps_, id, &WebApp::id);
    if (app == web_apps_.end()) {
        return std::unexpected(StoreError{"not_found", "no app " + id});
    }
    app->phase = std::move(phase);
    app->url = std::move(url);
    app->ready = ready;
    app->message = std::move(message);
    app->observed_generation = observed_generation;
    app->updated_at = now_iso8601();
    return {};
}

Result<std::vector<AgentTaskStreamScope>>
InMemoryStore::list_agent_task_stream_scopes(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<AgentTaskStreamScope> tasks;
    for (const auto& task : agent_tasks_) {
        if (task.customer_id == customer_id) {
            tasks.push_back({task.id, task.agent, task.finished_at, task.workspace_id});
        }
    }
    return tasks;
}

Result<std::vector<Build>> InMemoryStore::list_builds(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<Build> result;
    for (auto build = builds_.rbegin(); build != builds_.rend(); ++build) {
        if (build->customer_id == customer_id) {
            result.push_back(*build);
        }
    }
    return result;
}

Result<Build> InMemoryStore::get_build(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto build = std::ranges::find(builds_, id, &Build::id);
    if (build == builds_.end()) {
        return std::unexpected(StoreError{"not_found", "no build " + id});
    }
    return *build;
}

Result<Build> InMemoryStore::create_build(const NewBuild& input) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == input.workspace_id && workspace.customer_id == input.customer_id &&
                   workspace.desired != "deleted";
        })) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
    }
    if (std::ranges::none_of(customers_, [&](const Customer& customer) {
            return customer.id == input.customer_id;
        })) {
        return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
    }
    const auto repository = std::ranges::find(repositories_, input.repository, &Repository::id);
    if (repository == repositories_.end() || repository->customer_id != input.customer_id ||
        !repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"not_found", "no such repository in this customer"});
    }
    const std::string now = now_iso8601();
    Build build;
    build.id = new_uuid_v4();
    build.customer_id = input.customer_id;
    build.workspace_id = input.workspace_id;
    build.repository = input.repository;
    build.revision = input.revision;
    build.context = input.context;
    build.dockerfile = input.dockerfile;
    build.target = input.target;
    build.build_args = input.build_args;
    build.created_at = now;
    build.updated_at = now;
    builds_.push_back(build);
    return build;
}

Result<Build> InMemoryStore::cancel_build(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto build = std::ranges::find(builds_, id, &Build::id);
    if (build == builds_.end()) {
        return std::unexpected(StoreError{"not_found", "no build " + id});
    }
    if (build->desired == "cancelled" || build->phase == "Cancelled") {
        return std::unexpected(StoreError{"conflict", "the build is already cancelled"});
    }
    if (build->phase == "Succeeded" || build->phase == "Failed") {
        return std::unexpected(StoreError{"conflict", "the build has already finished"});
    }
    build->desired = "cancelled";
    build->updated_at = now_iso8601();
    return *build;
}

Result<void> InMemoryStore::set_build_status(const std::string& id, BuildStatus status) {
    std::lock_guard lock(mutex_);
    const auto build = std::ranges::find(builds_, id, &Build::id);
    if (build == builds_.end()) {
        return std::unexpected(StoreError{"not_found", "no build " + id});
    }
    build->phase = std::move(status.phase);
    build->digest = std::move(status.digest);
    build->size = status.size;
    build->message = std::move(status.message);
    build->token_hashes = std::move(status.token_hashes);
    build->updated_at = now_iso8601();
    return {};
}

Result<void> InMemoryStore::age_build(const std::string& id, std::int64_t seconds) {
    std::lock_guard lock(mutex_);
    const auto build = std::ranges::find(builds_, id, &Build::id);
    if (build == builds_.end()) {
        return std::unexpected(StoreError{"not_found", "no build " + id});
    }
    const auto created = utc_seconds(build->created_at.substr(0, 19) + "Z");
    if (!created) {
        return std::unexpected(StoreError{"invalid_input", "build time is unreadable"});
    }
    build->created_at = std::format("{:%FT%TZ}", *created - std::chrono::seconds(seconds));
    return {};
}

Result<std::vector<ManagedStore>> InMemoryStore::list_stores(const std::string& customer_id,
                                                             const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<ManagedStore> stores;
    for (const ManagedStore& store : stores_) {
        if (store.customer_id == customer_id &&
            (workspace_id.empty() || store.workspace_id == workspace_id) &&
            !(store.desired == "absent" && store.phase == "Deleted")) {
            stores.push_back(store);
        }
    }
    return stores;
}

Result<ManagedStore> InMemoryStore::get_store(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const ManagedStore& store : stores_) {
        if (store.id == id) {
            return store;
        }
    }
    return std::unexpected(StoreError{"not_found", "no store " + id});
}

Result<ManagedStore> InMemoryStore::create_store(const NewManagedStore& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == in.customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    for (const ManagedStore& store : stores_) {
        if (store.customer_id == in.customer_id && store.desired == "present" &&
            store.name == in.name) {
            return std::unexpected(
                StoreError{"conflict", "a store named " + in.name + " already exists"});
        }
    }
    ManagedStore store;
    store.id = new_uuid_v4();
    store.customer_id = in.customer_id;
    store.type = in.type;
    store.provider = in.provider;
    store.name = in.name;
    store.slug = managed_service_slug("st", store.id);
    store.size_gi = in.size_gi;
    store.region = in.region;
    store.is_public = in.is_public;
    store.desired = "present";
    store.created_by_taap_id = in.created_by_taap_id;
    store.workspace_id = in.workspace_id;
    store.created_at = store.updated_at = now_iso8601();
    stores_.push_back(store);
    return store;
}

Result<ManagedStore> InMemoryStore::withdraw_store(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (ManagedStore& store : stores_) {
        if (store.id != id) {
            continue;
        }
        if (store.desired != "present") {
            return std::unexpected(StoreError{"conflict", "the store is already deleted"});
        }
        store.desired = "absent";
        ++store.generation;
        store.updated_at = now_iso8601();
        return store;
    }
    return std::unexpected(StoreError{"not_found", "no store " + id});
}

Result<AgentLaunch> InMemoryStore::create_agent_launch(const NewAgentLaunch& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_,
                             [&](const Customer& value) { return value.id == in.customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    if ((in.mode != "job" && in.mode != "service") || in.agent_id.empty() || in.scopes.empty()) {
        return std::unexpected(StoreError{"invalid_input", "agent launch is invalid"});
    }
    AgentLaunch launch;
    launch.id = new_uuid_v4();
    launch.customer_id = in.customer_id;
    launch.agent_id = in.agent_id;
    launch.agent_name = in.agent_name;
    launch.mode = in.mode;
    launch.trigger = in.trigger.empty() ? "manual" : in.trigger;
    launch.input = in.input;
    launch.scopes = in.scopes;
    launch.slug = managed_service_slug("al", launch.id);
    launch.desired = "present";
    launch.created_by_user_id = in.created_by_user_id;
    launch.created_by_taap_id = in.created_by_taap_id;
    launch.workspace_id = in.workspace_id;
    launch.created_at = launch.updated_at = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    agent_launches_.push_back(launch);
    return launch;
}

Result<AgentLaunch> InMemoryStore::get_agent_launch(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_launches_, id, &AgentLaunch::id);
    if (found == agent_launches_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent launch " + id});
    }
    return *found;
}

Result<std::vector<AgentLaunch>> InMemoryStore::list_agent_launches(const std::string& customer_id,
                                                                    const std::string& agent_id,
                                                                    int limit,
                                                                    const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<AgentLaunch> launches;
    for (auto it = agent_launches_.rbegin(); it != agent_launches_.rend(); ++it) {
        if (it->customer_id == customer_id && (agent_id.empty() || it->agent_id == agent_id) &&
            (workspace_id.empty() || it->workspace_id == workspace_id)) {
            launches.push_back(*it);
        }
        if (limit > 0 && static_cast<int>(launches.size()) >= limit) {
            break;
        }
    }
    return launches;
}

Result<AgentLaunch> InMemoryStore::withdraw_agent_launch(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_launches_, id, &AgentLaunch::id);
    if (found == agent_launches_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent launch " + id});
    }
    if (found->desired != "absent") {
        found->desired = "absent";
        ++found->generation;
    }
    return *found;
}

namespace {

std::optional<std::chrono::sys_seconds> launch_created_at(const std::string& text) {
    return utc_seconds(text);
}

} // namespace

Result<std::size_t> InMemoryStore::expire_unstarted_agent_launches(std::int64_t seconds,
                                                                   const std::string& reason) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::size_t expired = 0;
    for (AgentLaunch& launch : agent_launches_) {
        const auto created = launch_created_at(launch.created_at);
        if (launch.desired != "present" || launch.observed_generation != 0 || !created ||
            now - *created < std::chrono::seconds(seconds)) {
            continue;
        }
        launch.desired = "absent";
        ++launch.generation;
        launch.stop_reason = reason;
        ++expired;
    }
    return expired;
}

Result<void> InMemoryStore::age_agent_launch(const std::string& id, std::int64_t seconds) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_launches_, id, &AgentLaunch::id);
    if (found == agent_launches_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent launch " + id});
    }
    const auto created = launch_created_at(found->created_at);
    if (!created) {
        return std::unexpected(StoreError{"invalid_input", "launch time is unreadable"});
    }
    found->created_at = std::format("{:%FT%TZ}", *created - std::chrono::seconds(seconds));
    return {};
}

Result<void> InMemoryStore::set_agent_launch_status(const std::string& id,
                                                    std::string phase,
                                                    std::string message,
                                                    std::string endpoint,
                                                    std::string credential_sha256,
                                                    std::optional<int> exit_code,
                                                    nlohmann::json result) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_launches_, id, &AgentLaunch::id);
    if (found == agent_launches_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent launch " + id});
    }
    found->phase = std::move(phase);
    found->message = std::move(message);
    found->endpoint = std::move(endpoint);
    found->credential_sha256 = std::move(credential_sha256);
    found->exit_code = exit_code;
    found->result = std::move(result);
    found->observed_generation = found->generation;
    found->status_updated_at = now_iso8601();
    return {};
}

Result<MethodologyRun> InMemoryStore::create_methodology_run(const NewMethodologyRun& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_,
                             [&](const Customer& value) { return value.id == in.customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    if ((in.mode != "validate" && in.mode != "train") || in.methodology_id.empty() ||
        in.runtime_image.empty() || in.cpu_millis <= 0 || in.memory_bytes <= 0 || in.gpus < 0 ||
        in.timeout_seconds <= 0) {
        return std::unexpected(StoreError{"invalid_input", "methodology run is invalid"});
    }
    const auto workspace = std::ranges::find_if(workspaces_, [&](const Workspace& value) {
        return value.id == in.workspace_id && value.customer_id == in.customer_id &&
               value.desired != "deleted";
    });
    if (workspace == workspaces_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "workspace_id must name a workspace in this space"});
    }
    MethodologyRun run;
    run.id = new_uuid_v4();
    run.customer_id = in.customer_id;
    run.workspace_id = in.workspace_id;
    run.methodology_id = in.methodology_id;
    run.mode = in.mode;
    run.build_id = in.build_id;
    run.image = in.image;
    run.runtime_image = in.runtime_image;
    run.cpu_millis = in.cpu_millis;
    run.memory_bytes = in.memory_bytes;
    run.gpus = in.gpus;
    run.gpu_class = in.gpu_class;
    run.timeout_seconds = in.timeout_seconds;
    run.network = in.network;
    run.slug = managed_service_slug("mr", run.id);
    run.desired = "present";
    run.created_by_user_id = in.created_by_user_id;
    run.created_by_taap_id = in.created_by_taap_id;
    run.created_at = run.updated_at = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    methodology_runs_.push_back(run);
    return run;
}

Result<MethodologyRun> InMemoryStore::get_methodology_run(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(methodology_runs_, id, &MethodologyRun::id);
    if (found == methodology_runs_.end()) {
        return std::unexpected(StoreError{"not_found", "no methodology run " + id});
    }
    return *found;
}

Result<std::vector<MethodologyRun>>
InMemoryStore::list_methodology_runs(const std::string& customer_id,
                                     const std::string& workspace_id,
                                     const std::string& methodology_id,
                                     int limit) {
    std::lock_guard lock(mutex_);
    std::vector<MethodologyRun> runs;
    for (auto it = methodology_runs_.rbegin(); it != methodology_runs_.rend(); ++it) {
        if (limit > 0 && static_cast<int>(runs.size()) >= limit) {
            break;
        }
        if (it->customer_id == customer_id && it->workspace_id == workspace_id &&
            (methodology_id.empty() || it->methodology_id == methodology_id)) {
            runs.push_back(*it);
        }
    }
    return runs;
}

Result<MethodologyRun> InMemoryStore::withdraw_methodology_run(const std::string& id,
                                                               const std::string& reason) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(methodology_runs_, id, &MethodologyRun::id);
    if (found == methodology_runs_.end()) {
        return std::unexpected(StoreError{"not_found", "no methodology run " + id});
    }
    if (found->desired == "absent") {
        return std::unexpected(StoreError{"conflict", "the run is already stopped"});
    }
    found->desired = "absent";
    ++found->generation;
    found->stop_reason = reason;
    found->updated_at = now_iso8601();
    return *found;
}

Result<std::size_t> InMemoryStore::expire_unstarted_methodology_runs(std::int64_t seconds,
                                                                     const std::string& reason) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::size_t expired = 0;
    for (MethodologyRun& run : methodology_runs_) {
        const auto created = utc_seconds(run.created_at);
        if (run.desired != "present" || !run.phase.empty() || !created ||
            now - *created < std::chrono::seconds(seconds)) {
            continue;
        }
        run.desired = "absent";
        ++run.generation;
        run.stop_reason = reason;
        ++expired;
    }
    return expired;
}

Result<void> InMemoryStore::put_methodology_run_status(const std::string& id,
                                                       const MethodologyRunStatus& status) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(methodology_runs_, id, &MethodologyRun::id);
    if (found == methodology_runs_.end()) {
        return std::unexpected(StoreError{"not_found", "no methodology run " + id});
    }
    found->phase = status.phase;
    found->message = status.message;
    found->exit_code = status.exit_code;
    found->credential_sha256 = status.credential_sha256;
    found->observed_generation =
        status.observed_generation ? status.observed_generation : found->generation;
    found->started_at = status.started_at;
    found->finished_at = status.finished_at;
    found->status_updated_at = now_iso8601();
    return {};
}

Result<void> InMemoryStore::age_methodology_run(const std::string& id, std::int64_t seconds) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(methodology_runs_, id, &MethodologyRun::id);
    if (found == methodology_runs_.end()) {
        return std::unexpected(StoreError{"not_found", "no methodology run " + id});
    }
    const auto created = utc_seconds(found->created_at);
    if (!created) {
        return std::unexpected(StoreError{"invalid_input", "run time is unreadable"});
    }
    found->created_at = std::format("{:%FT%TZ}", *created - std::chrono::seconds(seconds));
    return {};
}

Result<AgentCronSchedule>
InMemoryStore::create_agent_cron_schedule(const NewAgentCronSchedule& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_,
                             [&](const Customer& value) { return value.id == in.customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    if (in.agent_id.empty() || in.cron.empty() || in.scopes.empty()) {
        return std::unexpected(StoreError{"invalid_input", "agent schedule is invalid"});
    }
    AgentCronSchedule schedule;
    schedule.id = new_uuid_v4();
    schedule.customer_id = in.customer_id;
    schedule.agent_id = in.agent_id;
    schedule.agent_name = in.agent_name;
    schedule.cron = in.cron;
    schedule.time_zone = in.time_zone.empty() ? "Etc/UTC" : in.time_zone;
    schedule.description = in.description;
    schedule.input = in.input;
    schedule.scopes = in.scopes;
    schedule.slug = managed_service_slug("as", schedule.id);
    schedule.desired = "present";
    schedule.created_by_user_id = in.created_by_user_id;
    schedule.created_by_taap_id = in.created_by_taap_id;
    schedule.workspace_id = in.workspace_id;
    schedule.created_at = schedule.updated_at = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    agent_cron_schedules_.push_back(schedule);
    return schedule;
}

Result<AgentCronSchedule> InMemoryStore::get_agent_cron_schedule(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_cron_schedules_, id, &AgentCronSchedule::id);
    if (found == agent_cron_schedules_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent schedule " + id});
    }
    return *found;
}

Result<std::vector<AgentCronSchedule>>
InMemoryStore::list_agent_cron_schedules(const std::string& customer_id,
                                         const std::string& agent_id,
                                         const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<AgentCronSchedule> schedules;
    for (const AgentCronSchedule& schedule : agent_cron_schedules_) {
        if (schedule.customer_id == customer_id && schedule.desired == "present" &&
            (agent_id.empty() || schedule.agent_id == agent_id) &&
            (workspace_id.empty() || schedule.workspace_id == workspace_id)) {
            schedules.push_back(schedule);
        }
    }
    return schedules;
}

Result<AgentCronSchedule> InMemoryStore::withdraw_agent_cron_schedule(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_cron_schedules_, id, &AgentCronSchedule::id);
    if (found == agent_cron_schedules_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent schedule " + id});
    }
    if (found->desired == "absent") {
        return std::unexpected(StoreError{"conflict", "the schedule is already removed"});
    }
    found->desired = "absent";
    ++found->generation;
    return *found;
}

Result<std::vector<AgentLaunch>> InMemoryStore::list_agent_cron_runs(const std::string& customer_id,
                                                                     const std::string& agent_id,
                                                                     int limit,
                                                                     const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<AgentLaunch> runs;
    for (auto it = agent_cron_runs_.rbegin(); it != agent_cron_runs_.rend(); ++it) {
        if (it->second.customer_id == customer_id &&
            (agent_id.empty() || it->second.agent_id == agent_id) &&
            (workspace_id.empty() || it->second.workspace_id == workspace_id)) {
            runs.push_back(it->second);
        }
        if (limit > 0 && static_cast<int>(runs.size()) >= limit) {
            break;
        }
    }
    return runs;
}

Result<void> InMemoryStore::set_agent_cron_schedule_status(const std::string& id,
                                                           std::string phase,
                                                           std::string message,
                                                           std::string credential_sha256) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_cron_schedules_, id, &AgentCronSchedule::id);
    if (found == agent_cron_schedules_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent schedule " + id});
    }
    found->phase = std::move(phase);
    found->message = std::move(message);
    found->credential_sha256 = std::move(credential_sha256);
    found->observed_generation = found->generation;
    return {};
}

Result<void> InMemoryStore::put_agent_cron_run(const std::string& schedule_id,
                                               const std::string& run_id,
                                               const std::string& job_name,
                                               std::string phase,
                                               std::string message,
                                               std::optional<int> exit_code,
                                               nlohmann::json result) {
    std::lock_guard lock(mutex_);
    const auto schedule =
        std::ranges::find(agent_cron_schedules_, schedule_id, &AgentCronSchedule::id);
    if (schedule == agent_cron_schedules_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent schedule " + schedule_id});
    }
    auto run = std::ranges::find_if(agent_cron_runs_,
                                    [&](const auto& entry) { return entry.second.id == run_id; });
    if (run == agent_cron_runs_.end()) {
        AgentLaunch launch;
        launch.id = run_id;
        launch.customer_id = schedule->customer_id;
        launch.agent_id = schedule->agent_id;
        launch.agent_name = schedule->agent_name;
        launch.mode = "job";
        launch.trigger = "schedule";
        launch.input = schedule->input;
        launch.slug = job_name;
        launch.desired = "present";
        launch.workspace_id = schedule->workspace_id;
        launch.observed_generation = 1; // the operator wrote it
        launch.created_by_user_id = schedule->created_by_user_id;
        launch.created_by_taap_id = schedule->created_by_taap_id;
        launch.created_at = launch.updated_at =
            std::format("{:%FT%TZ}",
                        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        agent_cron_runs_.emplace_back(schedule_id, std::move(launch));
        run = std::prev(agent_cron_runs_.end());
    }
    run->second.phase = std::move(phase);
    run->second.message = std::move(message);
    run->second.exit_code = exit_code;
    run->second.result = std::move(result);
    return {};
}

Result<AgentTask> InMemoryStore::create_agent_task(const AgentTask& in) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == in.workspace_id && workspace.customer_id == in.customer_id &&
                   workspace.desired != "deleted";
        })) {
        return std::unexpected(StoreError{"invalid_input", "workspace_id must name a workspace Zetta in this account"});
    }
    if (in.agent.empty() || in.scopes.empty()) {
        return std::unexpected(StoreError{"invalid_input", "agent task is invalid"});
    }
    if (!in.id.empty() && std::ranges::any_of(agent_tasks_, [&](const AgentTask& existing) {
            return existing.id == in.id;
        })) {
        return std::unexpected(StoreError{"conflict", "agent task " + in.id + " exists"});
    }
    AgentTask task = in;
    if (task.id.empty()) {
        task.id = new_uuid_v4();
    }
    // The request, message and answer are records in the task's workspace (lake#374).
    task.body = nlohmann::json::object();
    task.message.clear();
    task.result = nullptr;
    task.desired = "present";
    task.phase.clear();
    task.created_at = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    agent_tasks_.push_back(task);
    const auto now = std::chrono::steady_clock::now();
    agent_task_clocks_[task.id] = {now, now, {}};
    return task;
}

namespace {
std::string utc_now() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}
} // namespace

Result<AgentTask> InMemoryStore::latest_agent_task(const std::string& customer_id,
                                                   const std::string& kind,
                                                   const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    for (auto task = agent_tasks_.rbegin(); task != agent_tasks_.rend(); ++task) {
        if (task->customer_id == customer_id && task->kind == kind && task->workspace_id == workspace_id) {
            return *task;
        }
    }
    return std::unexpected(StoreError{"not_found", "no " + kind + " task"});
}

Result<AgentTask> InMemoryStore::latest_agent_conversation_task(
    const std::string& customer_id,
    const std::string& conversation_id,
    const std::string& agent,
    const std::string& kind,
    const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    for (auto task = agent_tasks_.rbegin(); task != agent_tasks_.rend(); ++task) {
        if (task->customer_id == customer_id && task->conversation_id == conversation_id &&
            task->agent == agent && task->kind == kind && task->workspace_id == workspace_id) {
            return *task;
        }
    }
    return std::unexpected(StoreError{"not_found", "no earlier turn in this conversation"});
}

Result<AgentTask> InMemoryStore::claim_agent_task(const std::string& id,
                                                  const std::string& agent,
                                                  const std::string& credential_sha256,
                                                  std::uint64_t events_from,
                                                  int lease_seconds) {
    if (lease_seconds < 60 || lease_seconds > 3600) {
        return std::unexpected(StoreError{"invalid_input", "agent lease must be 60..3600 seconds"});
    }
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end() || found->desired != "present") {
        return std::unexpected(StoreError{"not_found", "no agent task " + id});
    }
    if (found->agent != agent || !found->phase.empty() || found->retry_notice_pending) {
        return std::unexpected(StoreError{"conflict", "the task is not waiting for " + agent});
    }
    found->phase = "Running";
    found->credential_sha256 = credential_sha256;
    if (found->events_from == 0) {
        found->events_from = events_from;
    }
    found->lease_seconds = lease_seconds;
    found->lease_until =
        iso8601_at(std::chrono::system_clock::now() + std::chrono::seconds(lease_seconds));
    found->started_at = utc_now();
    agent_task_clocks_[id].started = std::chrono::steady_clock::now();
    agent_task_clocks_[id].lease_until =
        std::chrono::steady_clock::now() + std::chrono::seconds(lease_seconds);
    return *found;
}

Result<void> InMemoryStore::renew_agent_task_lease(const std::string& id,
                                                   const std::string& credential_sha256) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end() || found->phase != "Running" || credential_sha256.empty() ||
        !constant_time_hex_equal(found->credential_sha256, credential_sha256)) {
        return std::unexpected(StoreError{"conflict", "the task claim is no longer active"});
    }
    const auto lease = std::chrono::seconds(found->lease_seconds);
    found->lease_until = iso8601_at(std::chrono::system_clock::now() + lease);
    agent_task_clocks_[id].lease_until = std::chrono::steady_clock::now() + lease;
    return {};
}

Result<void> InMemoryStore::mark_agent_task_notice_published(const std::string& id,
                                                             int attempts,
                                                             const std::string& phase) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found != agent_tasks_.end() && found->attempts == attempts && found->phase == phase &&
        found->retry_notice_pending) {
        found->retry_notice_pending = false;
        if (found->phase.empty()) {
            agent_task_clocks_[id].created = std::chrono::steady_clock::now();
        }
    }
    return {};
}

Result<AgentTask> InMemoryStore::record_agent_task_event(const std::string& id,
                                                         const std::string& agent,
                                                         const std::string& phase) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end() || found->agent != agent || found->phase != "Running") {
        return std::unexpected(StoreError{"conflict", "the task is not running on " + agent});
    }
    found->phase = phase;
    found->message.clear(); // lake's notice: the agent spoke since
    if (phase != "Running") {
        found->finished_at = utc_now();
        found->credential_sha256.clear(); // a finished task's credential stops
        found->lease_until.clear();
    } else {
        const auto lease = std::chrono::seconds(found->lease_seconds);
        found->lease_until = iso8601_at(std::chrono::system_clock::now() + lease);
        agent_task_clocks_[id].lease_until = std::chrono::steady_clock::now() + lease;
    }
    return *found;
}

Result<void> InMemoryStore::fail_agent_task(const std::string& id, const std::string& notice) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent task " + id});
    }
    const auto waiting = std::chrono::steady_clock::now() - agent_task_clocks_[id].created;
    if (found->phase.empty() && !found->retry_notice_pending &&
        waiting >= std::chrono::seconds(found->deadline_seconds)) {
        found->phase = "Failed";
        found->message = notice;
        found->finished_at = utc_now();
        found->credential_sha256.clear();
        found->lease_until.clear();
    }
    return {};
}

Result<void> InMemoryStore::mark_agent_task_published(const std::string& id) {
    std::lock_guard lock(mutex_);
    agent_task_clocks_[id].published = std::chrono::steady_clock::now();
    return {};
}

Result<StaleAgentTasks> InMemoryStore::stale_agent_tasks(std::chrono::seconds republish_after) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    StaleAgentTasks out;
    for (AgentTask& task : agent_tasks_) {
        TaskClock& clock = agent_task_clocks_[task.id];
        if (task.desired == "present" && task.phase == "Running" && clock.lease_until < now) {
            task.credential_sha256.clear();
            task.lease_until.clear();
            task.retry_notice_pending = true;
            task.message = "the agent stopped responding";
            if (task.attempts >= 3) {
                task.phase = "Failed";
                task.finished_at = utc_now();
            } else {
                ++task.attempts;
                task.phase.clear();
                task.message += "; retrying (attempt " + std::to_string(task.attempts) + " of 3)";
                clock.created = now;
                clock.published = now - republish_after;
            }
        }
        if (task.retry_notice_pending) {
            out.reclaimed.push_back(task);
            continue;
        }
        if (!task.finished_at.empty() || task.desired != "present") {
            continue;
        }
        const auto deadline = std::chrono::seconds(task.deadline_seconds);
        if (task.phase.empty()) {
            if (now - clock.created >= deadline) {
                out.overdue.push_back(task);
            } else if (now - clock.published >= republish_after) {
                out.unclaimed.push_back(task);
            }
        }
    }
    return out;
}

void InMemoryStore::age_agent_task(const std::string& id, std::chrono::seconds by) {
    std::lock_guard lock(mutex_);
    TaskClock& clock = agent_task_clocks_[id];
    clock.created -= by;
    clock.published -= by;
    clock.started -= by;
    clock.lease_until -= by;
    const auto task = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (task != agent_tasks_.end() && !task->lease_until.empty()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            clock.lease_until - std::chrono::steady_clock::now());
        task->lease_until = iso8601_at(std::chrono::system_clock::now() + remaining);
    }
}

Result<AgentTask> InMemoryStore::get_agent_task(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent task " + id});
    }
    return *found;
}

Result<void> InMemoryStore::set_agent_task_status(const std::string& id,
                                                  std::string phase,
                                                  std::string credential_sha256) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(agent_tasks_, id, &AgentTask::id);
    if (found == agent_tasks_.end()) {
        return std::unexpected(StoreError{"not_found", "no agent task " + id});
    }
    if (phase == "Succeeded" || phase == "Failed") {
        found->finished_at = utc_now();
    }
    found->phase = std::move(phase);
    found->credential_sha256 = std::move(credential_sha256);
    return {};
}

Result<void> InMemoryStore::set_store_status(const std::string& id,
                                             std::string phase,
                                             std::string message,
                                             std::string endpoint) {
    std::lock_guard lock(mutex_);
    for (ManagedStore& store : stores_) {
        if (store.id == id) {
            store.phase = std::move(phase);
            store.message = std::move(message);
            store.endpoint = std::move(endpoint);
            store.observed_generation = store.generation;
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "no store " + id});
}

Result<std::uint64_t> InMemoryStore::set_storage_quota(const std::string& customer_id,
                                                       std::uint64_t quota_bytes) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
    }
    storage_quotas_[customer_id] = quota_bytes;
    return quota_bytes;
}

Result<std::vector<UsageLimit>> InMemoryStore::usage_limits(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
    }
    std::vector<UsageLimit> rows;
    for (const auto& [key, limit] : usage_limits_) {
        if (key.first == customer_id) {
            rows.push_back(limit);
        }
    }
    return rows;
}

Result<UsageLimit> InMemoryStore::set_usage_limit(const UsageLimit& limit) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_, [&](const Customer& customer) {
            return customer.id == limit.customer_id;
        })) {
        return std::unexpected(StoreError{"not_found", "no customer " + limit.customer_id});
    }
    if (limit.dimension.empty() || limit.warn_percent < 1 || limit.warn_percent > 100) {
        return std::unexpected(StoreError{"invalid_input",
                                          "a dimension and a warning percentage "
                                          "from 1 to 100 are required"});
    }
    const auto key = std::make_pair(limit.customer_id, limit.dimension);
    if (limit.limit == 0) {
        usage_limits_.erase(key);
        return limit;
    }
    UsageLimit stored = limit;
    stored.updated_at = now_iso8601();
    usage_limits_[key] = stored;
    return stored;
}

namespace {

bool valid_plan_id(const std::string& id) {
    if (id.empty() || id.size() > 63 || id[0] < 'a' || id[0] > 'z') {
        return false;
    }
    return std::ranges::all_of(
        id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool valid_currency(const std::string& currency) {
    return currency.size() == 3 &&
           std::ranges::all_of(currency, [](char c) { return c >= 'A' && c <= 'Z'; });
}

} // namespace

Result<std::vector<BillingPlan>> InMemoryStore::billing_plans() {
    std::lock_guard lock(mutex_);
    std::vector<BillingPlan> plans;
    for (const auto& [id, plan] : billing_plans_) {
        plans.push_back(plan);
    }
    std::ranges::stable_sort(plans, [](const BillingPlan& left, const BillingPlan& right) {
        return std::tie(left.position, left.id) < std::tie(right.position, right.id);
    });
    return plans;
}

Result<BillingPlan> InMemoryStore::put_billing_plan(const BillingPlan& plan) {
    if (!valid_plan_id(plan.id) || plan.name.empty() || !valid_currency(plan.currency)) {
        return std::unexpected(StoreError{"invalid_input",
                                          "a plan needs an id of lower-case letters, digits and "
                                          "hyphens, a name and a three-letter currency"});
    }
    std::set<std::pair<std::string, std::string>> seen;
    for (const BillingPrice& price : plan.prices) {
        if (price.dimension.empty() || price.per == 0 ||
            !seen.insert({price.dimension, price.class_name}).second) {
            return std::unexpected(
                StoreError{"invalid_input",
                           "each price needs its own dimension and class and a `per` above 0"});
        }
    }
    std::lock_guard lock(mutex_);
    BillingPlan stored = plan;
    const auto found = billing_plans_.find(plan.id);
    stored.created_at = found == billing_plans_.end() ? now_iso8601() : found->second.created_at;
    stored.updated_at = now_iso8601();
    std::ranges::sort(stored.prices, [](const BillingPrice& left, const BillingPrice& right) {
        return std::tie(left.dimension, left.class_name) <
               std::tie(right.dimension, right.class_name);
    });
    billing_plans_[plan.id] = stored;
    return stored;
}

Result<std::vector<WorkloadResources>> InMemoryStore::workload_resources() {
    std::lock_guard lock(mutex_);
    std::vector<WorkloadResources> rows;
    for (const auto& [key, row] : workload_resources_) {
        rows.push_back(row);
    }
    return rows;
}

Result<void> InMemoryStore::put_workload_resources(const WorkloadResources& row) {
    static const std::set<std::string> kinds{
        "workspace", "service", "store", "app", "build", "agent_launch"};
    if (!kinds.contains(row.object_kind) || row.object_id.empty() || row.pods < 0 || row.gpus < 0) {
        return std::unexpected(StoreError{"invalid_input", "not a workload resources row"});
    }
    std::lock_guard lock(mutex_);
    WorkloadResources stored = row;
    stored.observed_at = now_iso8601();
    workload_resources_[{row.object_kind, row.object_id}] = std::move(stored);
    return {};
}

Result<Invoice> InMemoryStore::create_invoice(const Invoice& invoice) {
    if (invoice.customer_id.empty() || invoice.period.size() != 7 || invoice.plan_id.empty() ||
        !valid_currency(invoice.currency) || invoice.provider.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "an invoice needs an account, a month, a plan and a currency"});
    }
    std::lock_guard lock(mutex_);
    for (const Invoice& existing : invoices_) {
        if (existing.customer_id == invoice.customer_id && existing.period == invoice.period &&
            existing.state != "void") {
            return std::unexpected(StoreError{"conflict",
                                              "invoice " + std::to_string(existing.number) +
                                                  " already bills " + invoice.period});
        }
    }
    Invoice stored = invoice;
    stored.id = new_uuid_v4();
    stored.number = invoices_.size() + 1;
    stored.state = "due";
    stored.issued_at = now_iso8601();
    stored.paid_at.clear();
    stored.voided_at.clear();
    invoices_.push_back(stored);
    return stored;
}

Result<std::vector<Invoice>> InMemoryStore::list_invoices(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    std::vector<Invoice> rows;
    for (const Invoice& invoice : invoices_) {
        if (invoice.customer_id == customer_id) {
            rows.push_back(invoice);
        }
    }
    std::ranges::sort(rows, [](const Invoice& left, const Invoice& right) {
        return std::tie(right.period, right.number) < std::tie(left.period, left.number);
    });
    return rows;
}

Result<Invoice> InMemoryStore::get_invoice(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const Invoice& invoice : invoices_) {
        if (invoice.id == id) {
            return invoice;
        }
    }
    return std::unexpected(StoreError{"not_found", "no invoice " + id});
}

Result<Invoice> InMemoryStore::set_invoice_state(const std::string& id,
                                                 const std::string& state,
                                                 const std::string& provider_reference,
                                                 const std::string& note) {
    if (state != "paid" && state != "void") {
        return std::unexpected(StoreError{"invalid_input", "an invoice becomes paid or void"});
    }
    std::lock_guard lock(mutex_);
    for (Invoice& invoice : invoices_) {
        if (invoice.id != id) {
            continue;
        }
        if (invoice.state != "due") {
            return std::unexpected(StoreError{"conflict",
                                              "invoice " + std::to_string(invoice.number) + " is " +
                                                  invoice.state + ", not due"});
        }
        invoice.state = state;
        if (!provider_reference.empty()) {
            invoice.provider_reference = provider_reference;
        }
        invoice.note = note;
        (state == "paid" ? invoice.paid_at : invoice.voided_at) = now_iso8601();
        return invoice;
    }
    return std::unexpected(StoreError{"not_found", "no invoice " + id});
}

Result<std::string> InMemoryStore::billing_provider_customer(const std::string& customer_id,
                                                             const std::string& provider) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
    }
    const auto found = billing_provider_customers_.find({customer_id, provider});
    return found == billing_provider_customers_.end() ? std::string() : found->second;
}

Result<std::string>
InMemoryStore::set_billing_provider_customer(const std::string& customer_id,
                                             const std::string& provider,
                                             const std::string& provider_customer_id) {
    if (provider.empty() || provider_customer_id.empty() || provider_customer_id.size() > 255) {
        return std::unexpected(
            StoreError{"invalid_input", "a provider and its customer id are required"});
    }
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(
            customers_, [&](const Customer& customer) { return customer.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "no customer " + customer_id});
    }
    return billing_provider_customers_.try_emplace({customer_id, provider}, provider_customer_id)
        .first->second;
}

Result<PaymentEventResult> InMemoryStore::apply_payment_event(const PaymentEvent& event) {
    if (event.provider.empty() || event.event_id.empty() || event.event_id.size() > 255 ||
        (!event.state.empty() && event.state != "paid" && event.state != "void")) {
        return std::unexpected(StoreError{"invalid_input",
                                          "a payment event needs its provider, "
                                          "its id and a state of paid or void"});
    }
    std::lock_guard lock(mutex_);
    PaymentEventResult result;
    if (!payment_events_.insert({event.provider, event.event_id}).second) {
        result.duplicate = true;
        return result;
    }
    for (Invoice& invoice : invoices_) {
        if (event.reference.empty() || invoice.provider != event.provider ||
            invoice.provider_reference != event.reference) {
            continue;
        }
        result.matched = true;
        if (invoice.state == "due") {
            if (!event.state.empty()) {
                invoice.state = event.state;
                (event.state == "paid" ? invoice.paid_at : invoice.voided_at) = now_iso8601();
                if (!event.note.empty()) {
                    invoice.note = event.note;
                }
                result.changed = true;
            }
            if (!event.provider_status.empty() &&
                invoice.provider_status != event.provider_status) {
                invoice.provider_status = event.provider_status;
                result.changed = true;
            }
        } else if (!event.state.empty() && event.state == invoice.state &&
                   !event.provider_status.empty() &&
                   invoice.provider_status != event.provider_status) {
            // The provider confirms what lake already did (a void lake asked for).
            invoice.provider_status = event.provider_status;
            result.changed = true;
        }
        result.invoice = invoice;
        break;
    }
    return result;
}

Result<User> InMemoryStore::get_user(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& u : users_)
        if (u.id == id)
            return u;
    return std::unexpected(StoreError{"not_found", "no user " + id});
}

Result<std::map<std::string, std::string>>
InMemoryStore::principal_names(const std::vector<std::string>& taap_ids) {
    std::lock_guard lock(mutex_);
    std::map<std::string, std::string> names;
    for (const std::string& taap_id : taap_ids) {
        for (const User& user : users_) {
            if (!taap_id.empty() && user.taap_id == taap_id) {
                names[taap_id] = user.display_name.empty() ? user.email : user.display_name;
            }
        }
        for (const Organization& organization : organizations_) {
            if (!taap_id.empty() && organization.taap_id == taap_id &&
                !organization.display_name.empty()) {
                names[taap_id] = organization.display_name;
            }
        }
    }
    return names;
}

Result<Organization> InMemoryStore::create_organization(const NewOrganization& in) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_) {
        if (!in.taap_id.empty() && o.taap_id == in.taap_id)
            return std::unexpected(StoreError{"conflict", "taap_id already exists"});
    }
    const std::string now = now_iso8601();
    organizations_.push_back(Organization{new_uuid_v4(), in.taap_id, in.display_name, now, now});
    std::string parent_id;
    if (!in.customer_id.empty()) {
        const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
            return object.object_type == "customer" && object.external_id == in.customer_id;
        });
        if (parent == policy_objects_.end()) {
            organizations_.pop_back();
            return std::unexpected(
                StoreError{"invalid_input", "customer_id does not reference a policy object"});
        }
        parent_id = parent->id;
    }
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "organization", organizations_.back().id, parent_id, now, now, true});
    return organizations_.back();
}

Result<std::vector<Organization>> InMemoryStore::list_organizations(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Organization> out(organizations_.rbegin(), organizations_.rend());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Organization> InMemoryStore::get_organization(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& o : organizations_)
        if (o.id == id)
            return o;
    return std::unexpected(StoreError{"not_found", "no organization " + id});
}

Result<Customer> InMemoryStore::create_customer(const NewCustomer& in) {
    std::lock_guard lock(mutex_);
    const bool individual = in.customer_type == "individual";
    if (!individual && in.customer_type != "organization") {
        return std::unexpected(
            StoreError{"invalid_input", "customer_type must be individual or organization"});
    }
    if (individual ? (in.user_id.empty() || !in.organization_id.empty())
                   : (in.organization_id.empty() || !in.user_id.empty())) {
        return std::unexpected(StoreError{"invalid_input",
                                          "an individual has exactly user_id, an organization "
                                          "exactly organization_id (customer_type_check)"});
    }
    if (individual && std::none_of(users_.begin(), users_.end(), [&](const User& u) {
            return u.id == in.user_id;
        })) {
        return std::unexpected(StoreError{"invalid_input", "user_id does not reference a user"});
    }
    if (!individual &&
        std::none_of(organizations_.begin(), organizations_.end(), [&](const Organization& o) {
            return o.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!valid_handle(in.name))
        return std::unexpected(StoreError{"invalid_input", "name must match customer_name_format"});
    for (const auto& c : customers_) {
        if (c.name == in.name)
            return std::unexpected(
                StoreError{"conflict", "name already taken (customer_name_unique)"});
        if (individual && c.user_id == in.user_id)
            return std::unexpected(StoreError{"conflict", "this user already has a customer"});
        if (!individual && c.organization_id == in.organization_id)
            return std::unexpected(
                StoreError{"conflict", "this organization already has a customer"});
    }
    const std::string now = now_iso8601();
    customers_.push_back(Customer{
        new_uuid_v4(), in.customer_type, in.user_id, in.organization_id, in.name, now, now});
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "customer", customers_.back().id, {}, now, now, true});
    if (!individual) {
        for (auto& object : policy_objects_) {
            if (object.object_type == "organization" && object.external_id == in.organization_id) {
                object.parent_id = policy_objects_.back().id;
                object.updated_at = now;
            }
        }
    }
    return customers_.back();
}

Result<std::vector<Customer>> InMemoryStore::list_customers(int limit, const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<Customer> out;
    for (auto it = customers_.rbegin(); it != customers_.rend(); ++it) {
        if (user_id.empty() || it->user_id == user_id) {
            out.push_back(*it);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Customer> InMemoryStore::get_customer(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& c : customers_)
        if (c.id == id)
            return c;
    return std::unexpected(StoreError{"not_found", "no customer " + id});
}

Result<Customer> InMemoryStore::organization_customer(const std::string& organization_id) {
    std::lock_guard lock(mutex_);
    for (const auto& c : customers_)
        if (!organization_id.empty() && c.organization_id == organization_id)
            return c;
    return std::unexpected(StoreError{"not_found", "no account for organization " + organization_id});
}

} // namespace lake::control

namespace lake::control {

nlohmann::json to_json(const Device& d) {
    return {{"id", d.id},
            {"user_id", d.user_id},
            {"name", d.name},
            {"passkey_credential_id", nullable(d.passkey_credential_id)},
            {"passkey_binding_attestation", d.passkey_binding_attestation},
            {"revoked_at", nullable(d.revoked_at)},
            {"created_at", d.created_at},
            {"updated_at", d.updated_at}};
}

nlohmann::json to_json(const Session& s) {
    return {{"id", s.id},
            {"user_id", nullable(s.user_id)},
            {"device_id", nullable(s.device_id)},
            {"pool_id", s.pool_id},
            {"kind", s.kind},
            {"parent_session_id", nullable(s.parent_session_id)},
            {"name", s.name},
            {"scopes", s.scopes},
            {"reason", s.reason},
            {"expires_at", nullable(s.expires_at)},
            {"revoked_at", nullable(s.revoked_at)},
            {"created_at", s.created_at},
            {"last_used_at", nullable(s.last_used_at)}};
}

nlohmann::json to_json(const OfflineGrant& value) {
    return {{"id", value.id},
            {"user_id", value.user_id},
            {"principal_taap_id", value.principal_taap_id},
            {"device_id", value.device_id},
            {"scopes", value.scopes},
            {"issued_at", value.issued_at},
            {"expires_at", value.expires_at},
            {"revoked_at", nullable(value.revoked_at)},
            {"credential", value.credential}};
}

nlohmann::json to_json(const RecoveryAttempt& value) {
    return {{"id", value.id},
            {"user_id", value.user_id},
            {"organization_id", nullable(value.organization_id)},
            {"method", value.method},
            {"status", value.status},
            {"ready_at", value.ready_at},
            {"expires_at", value.expires_at},
            {"completed_at", nullable(value.completed_at)},
            {"approval_count", value.approval_count}};
}

nlohmann::json to_json(const RecoveryDelivery& value) {
    return {{"id", value.id},
            {"recovery_id", value.recovery_id},
            {"email", value.email},
            {"token", value.token},
            {"kind", value.kind},
            {"created_at", value.created_at},
            {"delivered_at", nullable(value.delivered_at)}};
}

nlohmann::json to_json(const RecoveryNotification& value) {
    return {{"id", value.id},
            {"recovery_id", value.recovery_id},
            {"user_id", value.user_id},
            {"device_id", value.device_id},
            {"kind", value.kind},
            {"created_at", value.created_at}};
}

nlohmann::json to_json(const RecoveryContact& value, bool include_wrapped_key) {
    nlohmann::json output = {{"repository_id", value.repository_id},
                             {"contact_taap_id", value.contact_taap_id},
                             {"created_at", value.created_at},
                             {"updated_at", value.updated_at}};
    if (include_wrapped_key) {
        output["wrapped_key"] = value.wrapped_key;
    }
    return output;
}

nlohmann::json to_json(const SsoProvider& provider) {
    return {{"id", provider.id},
            {"organization_id", provider.organization_id},
            {"pool_id", provider.pool_id},
            {"name", provider.name},
            {"protocol", provider.protocol},
            {"issuer", provider.issuer},
            {"metadata", provider.metadata},
            {"signing_certificate", provider.signing_certificate},
            {"client_id", provider.client_id},
            {"attribute_mapping", provider.attribute_mapping},
            {"jit_provisioning", provider.jit_provisioning},
            {"enabled", provider.enabled},
            {"tested_at", nullable(provider.tested_at)},
            {"created_at", provider.created_at},
            {"updated_at", provider.updated_at}};
}

nlohmann::json to_json(const FederationIssuer& issuer) {
    return {{"id", issuer.id},
            {"organization_id", issuer.organization_id},
            {"issuer", issuer.issuer},
            {"claim", issuer.claim},
            {"created_at", issuer.created_at}};
}

nlohmann::json to_json(const ScimUser& user) {
    return {{"provider_id", user.provider_id},
            {"external_id", user.external_id},
            {"user_id", user.user_id},
            {"active", user.active},
            {"attributes", user.attributes},
            {"created_at", user.created_at},
            {"updated_at", user.updated_at}};
}

nlohmann::json to_json(const ScimGroup& group) {
    return {{"provider_id", group.provider_id},
            {"external_id", group.external_id},
            {"team_id", group.team_id},
            {"display_name", group.display_name},
            {"attributes", group.attributes},
            {"created_at", group.created_at},
            {"updated_at", group.updated_at}};
}

nlohmann::json to_json(const ProvisioningLog& log) {
    return {{"id", log.id},
            {"provider_id", log.provider_id},
            {"resource", log.resource},
            {"operation", log.operation},
            {"external_id", log.external_id},
            {"outcome", log.outcome},
            {"details", log.details},
            {"created_at", log.created_at}};
}

namespace {

Device&
add_device(std::vector<Device>& devices, const std::string& user_id, const std::string& name) {
    const std::string now = now_iso8601();
    devices.push_back(
        Device{new_uuid_v4(), user_id, name.empty() ? "browser" : name, "", now, now});
    return devices.back();
}

IssuedSession add_session(std::vector<Session>& sessions,
                          const std::string& user_id,
                          const std::string& pool_id,
                          const std::string& device_id,
                          const std::string& kind,
                          const std::string& name,
                          const std::vector<std::string>& scopes,
                          const std::string& reason,
                          int lifetime) {
    const std::string secret = identity_crypto::random_secret();
    const std::string hash = identity_crypto::hash_secret(secret);
    if (secret.empty() || hash.empty())
        return {};
    const std::string now = now_iso8601();
    std::string expires_at;
    if (lifetime > 0) {
        const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() +
                            lifetime;
        expires_at = std::to_string(expiry);
    }
    sessions.push_back(Session{new_uuid_v4(),
                               user_id,
                               device_id,
                               pool_id,
                               kind,
                               name,
                               reason,
                               expires_at,
                               "",
                               now,
                               "",
                               scopes});
    sessions.back().reason += "\n" + hash;
    return {sessions.back(), secret};
}

bool active_session_lineage(const std::vector<Session>& sessions,
                            const std::vector<Device>& devices,
                            const Session& session) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const Session* current = &session;
    std::set<std::string> visited;
    while (current) {
        if (!visited.insert(current->id).second || !current->revoked_at.empty() ||
            current->user_id != session.user_id || current->pool_id != session.pool_id ||
            (!current->expires_at.empty() && std::stoll(current->expires_at) <= now)) {
            return false;
        }
        if (std::ranges::any_of(devices, [&](const Device& device) {
                return device.id == current->device_id && !device.revoked_at.empty();
            })) {
            return false;
        }
        if (current->parent_session_id.empty()) {
            return true;
        }
        const auto parent = std::ranges::find(sessions, current->parent_session_id, &Session::id);
        if (parent == sessions.end()) {
            return false;
        }
        current = &*parent;
    }
    return false;
}

std::string session_hash(const Session& session) {
    const auto newline = session.reason.rfind('\n');
    return newline == std::string::npos ? "" : session.reason.substr(newline + 1);
}

std::string session_reason(const Session& session) {
    const auto newline = session.reason.rfind('\n');
    return newline == std::string::npos ? session.reason : session.reason.substr(0, newline);
}

} // namespace

Result<IssuedSession> InMemoryStore::sign_up_password(const NewPasswordUser& input) {
    if (input.email.empty() || input.password.empty())
        return std::unexpected(StoreError{"invalid_input", "email and password are required"});
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    const auto application = std::ranges::find_if(
        applications_, [&](const Application& value) { return value.pool_id == pool; });
    if (pool != "lake" && application == applications_.end()) {
        return std::unexpected(StoreError{"invalid_input", "application pool does not exist"});
    }
    for (auto& credential : passwords_) {
        if (credential.pool_id == pool && lower(credential.email) == lower(input.email))
            return std::unexpected(StoreError{"conflict", "email already exists"});
    }
    const std::string identity_secret = identity_crypto::random_secret();
    if (identity_secret.empty())
        return std::unexpected(StoreError{"store_error", "could not create identity"});
    const std::string identity = "taap:1:ed25519:" + identity_secret;
    const std::string now = now_iso8601();
    if (!input.user_id.empty() &&
        std::ranges::any_of(users_, [&](const User& value) { return value.id == input.user_id; })) {
        return std::unexpected(StoreError{"conflict", "user already exists"});
    }
    const User user{input.user_id.empty() ? new_uuid_v4() : input.user_id,
                    identity,
                    input.email,
                    input.display_name,
                    now,
                    now};
    const std::string password_hash = identity_crypto::hash_secret(input.password);
    if (password_hash.empty())
        return std::unexpected(StoreError{"store_error", "could not protect password"});
    std::vector<Device> devices;
    std::vector<Session> sessions;
    const Device& device = add_device(devices, user.id, input.device_name);
    IssuedSession issued =
        add_session(sessions, user.id, pool, device.id, "browser", "", {"*"}, "", 86400);
    if (issued.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    }
    std::string handle;
    if (pool == "lake") {
        do {
            handle = generate_handle();
        } while (std::ranges::any_of(
            customers_, [&](const Customer& customer) { return customer.name == handle; }));
    }
    users_.push_back(user);
    passwords_.push_back(
        PasswordCredential{users_.back().id, pool, input.email, password_hash, ""});
    if (pool == "lake") {
        customers_.push_back(Customer{new_uuid_v4(), "individual", user.id, "", handle, now, now});
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "customer", customers_.back().id, "", now, now, true});
        grants_.push_back(Grant{new_uuid_v4(),
                                identity,
                                policy_objects_.back().id,
                                "admin",
                                "sign_up_password/" + user.id,
                                identity,
                                now,
                                now});
    }
    devices_.push_back(devices.front());
    sessions_.push_back(sessions.front());
    issued.session.reason.clear();
    issued.created_user = true;
    return issued;
}

Result<IssuedSession> InMemoryStore::sign_in_password(const PasswordSignIn& input) {
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    for (const auto& credential : passwords_) {
        if (credential.pool_id != pool || lower(credential.email) != lower(input.email))
            continue;
        if (!identity_crypto::verify_secret(credential.password, input.password))
            return std::unexpected(StoreError{"unauthorized", "invalid email or password"});
        if (!credential.totp_secret.empty())
            return std::unexpected(StoreError{"mfa_required", "TOTP verification is required"});
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid email or password"});
}

Result<IssuedSession> InMemoryStore::sign_in_taap(const TaapSignIn& input) {
    if (input.pool_id.empty() || input.taap_id.empty() || input.assertion_id.empty()) {
        return std::unexpected(StoreError{"invalid_input", "TAAP sign-in fields are required"});
    }
    std::lock_guard lock(mutex_);
    if (!taap_assertions_.insert(input.assertion_id).second) {
        return std::unexpected(StoreError{"unauthorized", "TAAP assertion was already used"});
    }
    const auto user = std::ranges::find_if(
        users_, [&](const User& value) { return value.taap_id == input.taap_id; });
    const auto credential = std::ranges::find_if(passwords_, [&](const PasswordCredential& value) {
        return user != users_.end() && value.user_id == user->id && value.pool_id == input.pool_id;
    });
    if (user == users_.end() || credential == passwords_.end()) {
        return std::unexpected(StoreError{"unauthorized", "TAAP identity is not registered"});
    }
    Device& device = add_device(devices_, user->id, input.device_name);
    IssuedSession issued =
        add_session(sessions_, user->id, input.pool_id, device.id, "taap", "", {"*"}, "", 86400);
    if (issued.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    }
    issued.session.reason.clear();
    return issued;
}

Result<MfaEnrollment> InMemoryStore::enroll_mfa(const std::string& user_id,
                                                const std::string& pool_id) {
    MfaEnrollment enrollment;
    enrollment.secret = identity_crypto::new_totp_secret();
    if (enrollment.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not create TOTP secret"});
    }
    for (int index = 0; index < 8; ++index) {
        const std::string code = identity_crypto::random_secret().substr(0, 12);
        if (code.size() != 12) {
            return std::unexpected(
                StoreError{"store_error", "could not create MFA recovery codes"});
        }
        enrollment.recovery_codes.push_back(code);
    }
    std::lock_guard lock(mutex_);
    for (auto& credential : passwords_) {
        if (credential.user_id == user_id && credential.pool_id == pool_id) {
            credential.totp_secret = enrollment.secret;
            credential.recovery_hashes.clear();
            for (const std::string& code : enrollment.recovery_codes) {
                const std::string hash = identity_crypto::hash_secret(code);
                if (hash.empty()) {
                    credential.totp_secret.clear();
                    credential.recovery_hashes.clear();
                    return std::unexpected(
                        StoreError{"store_error", "could not protect MFA recovery codes"});
                }
                credential.recovery_hashes.push_back(hash);
            }
            return enrollment;
        }
    }
    return std::unexpected(StoreError{"not_found", "password credential not found"});
}

Result<bool> InMemoryStore::mfa_enrolled(const std::string& user_id, const std::string& pool_id) {
    std::lock_guard lock(mutex_);
    const std::string pool = pool_id.empty() ? "lake" : pool_id;
    for (const auto& credential : passwords_) {
        if (credential.user_id == user_id && credential.pool_id == pool &&
            !credential.totp_secret.empty()) {
            return true;
        }
    }
    return false;
}

Result<void> InMemoryStore::disable_mfa(const std::string& user_id,
                                        const std::string& pool_id,
                                        const std::string& code) {
    std::lock_guard lock(mutex_);
    for (auto& credential : passwords_) {
        if (credential.user_id != user_id || credential.pool_id != pool_id ||
            credential.totp_secret.empty()) {
            continue;
        }
        const bool valid_totp = identity_crypto::verify_totp(credential.totp_secret, code);
        const auto recovery = std::find_if(
            credential.recovery_hashes.begin(),
            credential.recovery_hashes.end(),
            [&](const std::string& hash) { return identity_crypto::verify_secret(hash, code); });
        if (valid_totp || recovery != credential.recovery_hashes.end()) {
            credential.totp_secret.clear();
            credential.recovery_hashes.clear();
            return {};
        }
    }
    return std::unexpected(StoreError{"unauthorized", "a fresh MFA verification is required"});
}

Result<IssuedSession> InMemoryStore::sign_in_totp(const PasswordSignIn& input) {
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    for (auto& credential : passwords_) {
        if (credential.pool_id != pool || lower(credential.email) != lower(input.email))
            continue;
        const bool valid_totp =
            identity_crypto::verify_totp(credential.totp_secret, input.totp_code);
        auto recovery =
            std::find_if(credential.recovery_hashes.begin(),
                         credential.recovery_hashes.end(),
                         [&](const std::string& hash) {
                             return identity_crypto::verify_secret(hash, input.totp_code);
                         });
        if (!identity_crypto::verify_secret(credential.password, input.password) ||
            (!valid_totp && recovery == credential.recovery_hashes.end())) {
            return std::unexpected(StoreError{"unauthorized", "invalid credentials or TOTP code"});
        }
        if (!valid_totp) {
            credential.recovery_hashes.erase(recovery);
        }
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid credentials or TOTP code"});
}

Result<WebAuthnChallenge> InMemoryStore::begin_passkey_registration(const std::string& user_id,
                                                                    const std::string& pool_id,
                                                                    const std::string& rp_id,
                                                                    const std::string& origin) {
    if (user_id.empty() || pool_id.empty() || rp_id.empty() || origin.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "WebAuthn registration scope is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            users_.begin(), users_.end(), [&](const User& user) { return user.id == user_id; })) {
        return std::unexpected(StoreError{"not_found", "user not found"});
    }
    WebAuthnChallenge challenge{
        user_id, pool_id, "registration", identity_crypto::random_secret(), rp_id, origin};
    if (challenge.challenge.empty()) {
        return std::unexpected(StoreError{"store_error", "could not generate WebAuthn challenge"});
    }
    webauthn_challenges_.push_back(
        {challenge, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return challenge;
}

Result<void> InMemoryStore::register_passkey(const PasskeyRegistration& input) {
    identity_crypto::VerifiedPasskeyRegistration verified;
    std::string subject_taap_id;
    {
        std::lock_guard lock(mutex_);
        WebAuthnChallengeRecord* challenge = nullptr;
        for (WebAuthnChallengeRecord& candidate : webauthn_challenges_) {
            if (candidate.challenge.challenge == input.challenge &&
                candidate.challenge.ceremony == "registration" &&
                candidate.challenge.user_id == input.user_id &&
                candidate.challenge.pool_id == input.pool_id && !candidate.used &&
                candidate.expires_at > std::chrono::steady_clock::now()) {
                candidate.used = true;
                challenge = &candidate;
                break;
            }
        }
        if (challenge == nullptr) {
            return std::unexpected(
                StoreError{"unauthorized", "WebAuthn challenge is invalid or expired"});
        }
        const auto registration =
            identity_crypto::verify_passkey_registration(input.client_data_json,
                                                         input.attestation_object,
                                                         input.challenge,
                                                         challenge->challenge.rp_id,
                                                         challenge->challenge.origin);
        if (!registration) {
            return std::unexpected(StoreError{"unauthorized", registration.error()});
        }
        verified = *registration;
        for (const auto& credential : passkeys_) {
            if (credential.pool_id == input.pool_id &&
                credential.credential_id == verified.credential_id) {
                return std::unexpected(StoreError{"conflict", "passkey already registered"});
            }
        }
        const auto user = std::find_if(users_.begin(), users_.end(), [&](const User& candidate) {
            return candidate.id == input.user_id;
        });
        if (user == users_.end() || user->taap_id.empty()) {
            return std::unexpected(StoreError{"not_found", "user TAAP identity not found"});
        }
        subject_taap_id = user->taap_id;
    }
    const auto binding = audit::sign_passkey_binding(
        *this, subject_taap_id, verified.credential_id, input.pool_id, input.identity_options);
    if (!binding) {
        return std::unexpected(StoreError{binding.error().code, binding.error().message});
    }
    std::lock_guard lock(mutex_);
    for (const auto& credential : passkeys_) {
        if (credential.pool_id == input.pool_id &&
            credential.credential_id == verified.credential_id) {
            return std::unexpected(StoreError{"conflict", "passkey already registered"});
        }
    }
    Device& device = add_device(devices_, input.user_id, input.name);
    passkeys_.push_back(PasskeyCredential{input.user_id,
                                          input.pool_id,
                                          verified.credential_id,
                                          verified.public_key_pem,
                                          input.name,
                                          device.id,
                                          "",
                                          *binding,
                                          verified.sign_count});
    return {};
}

Result<WebAuthnChallenge> InMemoryStore::begin_passkey_sign_in(const std::string& pool_id,
                                                               const std::string& rp_id,
                                                               const std::string& origin) {
    if (pool_id.empty() || rp_id.empty() || origin.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "WebAuthn assertion scope is incomplete"});
    }
    std::lock_guard lock(mutex_);
    WebAuthnChallenge challenge{
        "", pool_id, "assertion", identity_crypto::random_secret(), rp_id, origin};
    if (challenge.challenge.empty()) {
        return std::unexpected(StoreError{"store_error", "could not generate WebAuthn challenge"});
    }
    webauthn_challenges_.push_back(
        {challenge, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return challenge;
}

Result<IssuedSession> InMemoryStore::sign_in_passkey(const PasskeyAssertion& input) {
    std::lock_guard lock(mutex_);
    WebAuthnChallengeRecord* challenge = nullptr;
    for (WebAuthnChallengeRecord& candidate : webauthn_challenges_) {
        if (candidate.challenge.challenge == input.challenge &&
            candidate.challenge.ceremony == "assertion" &&
            candidate.challenge.pool_id == input.pool_id && !candidate.used &&
            candidate.expires_at > std::chrono::steady_clock::now()) {
            candidate.used = true;
            challenge = &candidate;
            break;
        }
    }
    if (challenge == nullptr) {
        return std::unexpected(
            StoreError{"unauthorized", "WebAuthn challenge is invalid or expired"});
    }
    for (PasskeyCredential& credential : passkeys_) {
        if (credential.pool_id != input.pool_id ||
            credential.credential_id != input.credential_id || !credential.revoked_at.empty()) {
            continue;
        }
        const auto sign_count =
            identity_crypto::verify_passkey_assertion(input.client_data_json,
                                                      input.authenticator_data,
                                                      input.signature,
                                                      credential.public_key_pem,
                                                      input.challenge,
                                                      challenge->challenge.rp_id,
                                                      challenge->challenge.origin);
        if (!sign_count) {
            return std::unexpected(StoreError{"unauthorized", sign_count.error()});
        }
        if ((*sign_count != 0 || credential.sign_count != 0) &&
            *sign_count <= credential.sign_count) {
            return std::unexpected(
                StoreError{"unauthorized", "passkey sign count did not advance"});
        }
        credential.sign_count = *sign_count;
        Device& device = add_device(devices_, credential.user_id, input.device_name);
        IssuedSession issued = add_session(sessions_,
                                           credential.user_id,
                                           input.pool_id,
                                           device.id,
                                           "browser",
                                           "",
                                           {"*"},
                                           "",
                                           86400);
        if (issued.secret.empty())
            return std::unexpected(StoreError{"store_error", "could not issue session"});
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "unknown passkey"});
}

Result<IssuedSession> InMemoryStore::sign_in_oauth(const OAuthIdentity& input) {
    if (input.provider.empty() || input.subject.empty())
        return std::unexpected(StoreError{"invalid_input", "provider and subject are required"});
    std::lock_guard lock(mutex_);
    const std::string pool = input.pool_id.empty() ? "lake" : input.pool_id;
    const auto application = std::ranges::find_if(
        applications_, [&](const Application& value) { return value.pool_id == pool; });
    if (pool != "lake" && application == applications_.end()) {
        return std::unexpected(StoreError{"invalid_input", "application pool does not exist"});
    }
    const std::string subject = input.provider + ":" + input.subject;
    for (const auto& credential : oauth_credentials_) {
        if (credential.pool_id == pool && credential.subject == subject) {
            Device& device = add_device(devices_, credential.user_id, input.device_name);
            IssuedSession issued = add_session(
                sessions_, credential.user_id, pool, device.id, "browser", "", {"*"}, "", 86400);
            if (issued.secret.empty())
                return std::unexpected(StoreError{"store_error", "could not issue session"});
            issued.session.reason.clear();
            return issued;
        }
    }
    if (input.identity_document.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "a TAAP identity document is required for sign-up"});
    }
    const std::string now = now_iso8601();
    const std::string identity = "taap:1:ed25519:" + identity_crypto::random_secret();
    users_.push_back(User{new_uuid_v4(), identity, input.email, input.display_name, now, now});
    oauth_credentials_.push_back(OAuthCredential{users_.back().id, pool, subject});
    Device& device = add_device(devices_, users_.back().id, input.device_name);
    IssuedSession issued =
        add_session(sessions_, users_.back().id, pool, device.id, "browser", "", {"*"}, "", 86400);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    issued.session.reason.clear();
    issued.created_user = true;
    return issued;
}

Result<IssuedSession> InMemoryStore::sign_in_clerk(const ClerkIdentity& input) {
    if (input.subject.empty()) {
        return std::unexpected(StoreError{"invalid_input", "a Clerk subject is required"});
    }
    std::lock_guard lock(mutex_);
    const std::string subject = "clerk:" + input.subject;
    std::string user_id;
    for (const auto& credential : oauth_credentials_) {
        if (credential.pool_id == "lake" && credential.subject == subject) {
            user_id = credential.user_id;
        }
    }
    if (user_id.empty() && !input.email.empty()) {
        for (const auto& user : users_) {
            if (lower(user.email) == lower(input.email)) {
                user_id = user.id;
            }
        }
        if (!user_id.empty()) {
            oauth_credentials_.push_back(OAuthCredential{user_id, "lake", subject});
        }
    }
    if (user_id.empty()) {
        if (input.custodial_taap_id.empty() || input.custodial_public_key.empty() ||
            input.custodial_wrapped_seed.empty()) {
            return std::unexpected(
                StoreError{"invalid_input", "a custodial identity is required to create a user"});
        }
        const std::string now = now_iso8601();
        users_.push_back(User{
            new_uuid_v4(), input.custodial_taap_id, input.email, input.display_name, now, now});
        user_id = users_.back().id;
        custodial_identities_[user_id] = input;
        oauth_credentials_.push_back(OAuthCredential{user_id, "lake", subject});
        std::string handle;
        do {
            handle = generate_handle();
        } while (std::ranges::any_of(
            customers_, [&](const Customer& customer) { return customer.name == handle; }));
        customers_.push_back(Customer{new_uuid_v4(), "individual", user_id, "", handle, now, now});
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "customer", customers_.back().id, "", now, now, true});
        grants_.push_back(Grant{new_uuid_v4(),
                                input.custodial_taap_id,
                                policy_objects_.back().id,
                                "admin",
                                subject,
                                input.custodial_taap_id,
                                now,
                                now});
    }
    Device& device = add_device(devices_, user_id, input.device_name);
    IssuedSession issued =
        add_session(sessions_, user_id, "lake", device.id, "browser", "", {"*"}, "", 86400);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue session"});
    issued.session.reason.clear();
    return issued;
}

Result<void> InMemoryStore::register_oauth_provider(const OAuthProvider& input) {
    if (input.pool_id.empty() || input.provider.empty() || input.issuer.empty() ||
        input.client_id.empty() || input.client_secret.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "pool, provider, issuer, client id, and client secret are required"});
    }
    std::lock_guard lock(mutex_);
    for (OAuthProvider& provider : oauth_providers_) {
        if (provider.pool_id == input.pool_id && provider.provider == input.provider) {
            provider = input;
            return {};
        }
    }
    oauth_providers_.push_back(input);
    return {};
}

Result<OAuthProvider> InMemoryStore::get_oauth_provider(const std::string& pool_id,
                                                        const std::string& provider) {
    std::lock_guard lock(mutex_);
    for (const OAuthProvider& candidate : oauth_providers_) {
        if (candidate.pool_id == pool_id && candidate.provider == provider) {
            return candidate;
        }
    }
    return std::unexpected(StoreError{"not_found", "OAuth provider is not registered"});
}

Result<void> InMemoryStore::begin_oauth(const OAuthFlow& input) {
    if (input.pool_id.empty() || input.provider.empty() || input.state.empty() ||
        input.code_verifier.empty() || input.nonce.empty()) {
        return std::unexpected(StoreError{"invalid_input", "OAuth flow material is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const bool provider_exists = std::any_of(
        oauth_providers_.begin(), oauth_providers_.end(), [&](const OAuthProvider& provider) {
            return provider.pool_id == input.pool_id && provider.provider == input.provider;
        });
    const bool sso_provider_exists =
        input.provider.starts_with("sso-") &&
        std::ranges::any_of(sso_providers_, [&](const SsoProvider& provider) {
            return provider.id == input.provider.substr(std::string("sso-").size()) &&
                   provider.pool_id == input.pool_id && provider.protocol == "oidc" &&
                   provider.enabled;
        });
    if (!provider_exists && !sso_provider_exists) {
        return std::unexpected(StoreError{"not_found", "OAuth provider is not registered"});
    }
    oauth_flows_.push_back(
        {input, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return {};
}

Result<OAuthFlow> InMemoryStore::consume_oauth(const std::string& provider,
                                               const std::string& state) {
    std::lock_guard lock(mutex_);
    for (OAuthFlowRecord& record : oauth_flows_) {
        if (record.flow.provider == provider && record.flow.state == state && !record.used &&
            record.expires_at > std::chrono::steady_clock::now()) {
            record.used = true;
            return record.flow;
        }
    }
    return std::unexpected(StoreError{"unauthorized", "OAuth state is invalid or expired"});
}

Result<IssuedSession> InMemoryStore::create_token(const NewToken& input) {
    if (input.name.empty())
        return std::unexpected(StoreError{"invalid_input", "name is required"});
    if (input.is_static && input.reason.empty())
        return std::unexpected(StoreError{"invalid_input", "a static token requires a reason"});
    std::lock_guard lock(mutex_);
    std::string parent_expiry;
    if (!input.parent_session_id.empty()) {
        const auto parent = std::ranges::find(sessions_, input.parent_session_id, &Session::id);
        if (parent == sessions_.end() || parent->user_id != input.user_id ||
            parent->pool_id != input.pool_id ||
            !active_session_lineage(sessions_, devices_, *parent)) {
            return std::unexpected(StoreError{"unauthorized", "issuing session is not active"});
        }
        parent_expiry = parent->expires_at;
    }
    const int lifetime = input.is_static ? 0
                                         : (input.expires_in_seconds > 0 ? input.expires_in_seconds
                                                                         : 60 * 60 * 24 * 30);
    IssuedSession issued = add_session(sessions_,
                                       input.user_id,
                                       input.pool_id,
                                       "",
                                       input.is_static ? "static" : "api_token",
                                       input.name,
                                       input.scopes,
                                       input.reason,
                                       lifetime);
    if (issued.secret.empty())
        return std::unexpected(StoreError{"store_error", "could not issue token"});
    sessions_.back().parent_session_id = input.parent_session_id;
    if (!parent_expiry.empty() &&
        (issued.session.expires_at.empty() ||
         std::stoll(issued.session.expires_at) > std::stoll(parent_expiry))) {
        sessions_.back().expires_at = parent_expiry;
    }
    issued.session = sessions_.back();
    issued.session.reason = input.reason;
    return issued;
}

Result<std::vector<Session>> InMemoryStore::list_sessions(const std::string& user_id,
                                                          const std::string& pool_id) {
    std::lock_guard lock(mutex_);
    std::vector<Session> result;
    for (auto it = sessions_.rbegin(); it != sessions_.rend(); ++it) {
        if (it->user_id == user_id && it->pool_id == pool_id) {
            Session copy = *it;
            copy.reason = session_reason(copy);
            result.push_back(std::move(copy));
        }
    }
    return result;
}

Result<void> InMemoryStore::revoke_session(const std::string& user_id,
                                           const std::string& session_id) {
    std::lock_guard lock(mutex_);
    for (auto& session : sessions_)
        if (session.id == session_id && session.user_id == user_id) {
            session.revoked_at = now_iso8601();
            return {};
        }
    return std::unexpected(StoreError{"not_found", "session not found"});
}

Result<std::vector<Device>> InMemoryStore::list_devices(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<Device> result;
    for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
        if (it->user_id == user_id) {
            result.push_back(*it);
            const auto passkey =
                std::find_if(passkeys_.begin(), passkeys_.end(), [&](const auto& credential) {
                    return credential.device_id == it->id;
                });
            if (passkey != passkeys_.end()) {
                result.back().passkey_credential_id = passkey->credential_id;
                result.back().passkey_binding_attestation = passkey->binding_attestation;
            }
        }
    }
    return result;
}

Result<void> InMemoryStore::revoke_device(const std::string& user_id,
                                          const std::string& device_id) {
    std::lock_guard lock(mutex_);
    for (auto& device : devices_) {
        if (device.id == device_id && device.user_id == user_id) {
            device.revoked_at = now_iso8601();
            device.updated_at = device.revoked_at;
            for (PasskeyCredential& passkey : passkeys_) {
                if (passkey.device_id == device_id && passkey.revoked_at.empty()) {
                    passkey.revoked_at = device.revoked_at;
                }
            }
            return {};
        }
    }
    return std::unexpected(StoreError{"not_found", "device not found"});
}

Result<Principal> InMemoryStore::context_for(const std::string& secret) {
    std::lock_guard lock(mutex_);
    const std::string workload_digest = identity_crypto::sha256_hex(secret);
    std::optional<Principal> build_principal;
    for (const Build& build : builds_) {
        const auto purpose = build_token_purpose(build, workload_digest);
        if (!purpose) {
            continue;
        }
        if (build_principal) {
            return std::unexpected(StoreError{"unauthorized", "build token is ambiguous"});
        }
        build_principal = Principal{"",
                                    "build:" + build.id + ":" + *purpose,
                                    "build:" + build.id,
                                    "",
                                    "lake",
                                    "build_" + *purpose,
                                    build_token_scopes(*purpose),
                                    std::nullopt,
                                    build.id,
                                    build.customer_id,
                                    build.repository,
                                    build.revision,
                                    workload_digest};
    }
    if (build_principal) {
        return *build_principal;
    }
    std::optional<Principal> app_principal;
    for (const WebApp& app : web_apps_) {
        const auto purpose = web_app_token_purpose(app, workload_digest);
        if (!purpose) {
            continue;
        }
        if (app_principal) {
            return std::unexpected(StoreError{"unauthorized", "app token is ambiguous"});
        }
        app_principal = Principal{"",
                                  "app:" + app.id + ":" + *purpose,
                                  "app:" + app.id,
                                  "",
                                  "lake",
                                  "app_" + *purpose,
                                  web_app_token_scopes(*purpose),
                                  std::nullopt,
                                  app.id,
                                  app.customer_id,
                                  app.source_repository,
                                  app.source_revision,
                                  workload_digest};
    }
    if (app_principal) {
        return *app_principal;
    }
    if (secret.starts_with(agent_task_credential_prefix)) {
        const auto parsed = parse_agent_credential(secret, agent_task_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto task = std::ranges::find(agent_tasks_, parsed->first, &AgentTask::id);
        if (task == agent_tasks_.end() || task->desired != "present" ||
            task->credential_sha256.empty() ||
            !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                     task->credential_sha256)) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto user = std::ranges::find_if(
            users_, [&](const User& row) { return row.id == task->requested_by_user_id; });
        if (user == users_.end() || user->taap_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "task identity is unavailable"});
        }
        return Principal{user->id,
                         user->taap_id,
                         "task:" + task->id,
                         "",
                         "lake",
                         "agent_task",
                         task->scopes,
                         std::nullopt,
                         task->id,
                         task->customer_id,
                         "",
                         "",
                         task->credential_sha256};
    }
    if (secret.starts_with(agent_schedule_credential_prefix)) {
        const auto parsed = parse_agent_credential(secret, agent_schedule_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto schedule =
            std::ranges::find(agent_cron_schedules_, parsed->first, &AgentCronSchedule::id);
        if (schedule == agent_cron_schedules_.end() || schedule->desired != "present" ||
            schedule->credential_sha256.empty() ||
            !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                     schedule->credential_sha256)) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto user = std::ranges::find_if(
            users_, [&](const User& row) { return row.id == schedule->created_by_user_id; });
        if (user == users_.end() || user->taap_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "schedule identity is unavailable"});
        }
        return Principal{user->id,
                         user->taap_id,
                         "schedule:" + schedule->id,
                         "",
                         "lake",
                         "agent_schedule",
                         schedule->scopes,
                         std::nullopt};
    }
    if (secret.starts_with(methodology_run_credential_prefix)) {
        const auto parsed = parse_agent_credential(secret, methodology_run_credential_prefix);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto run = std::ranges::find(methodology_runs_, parsed->first, &MethodologyRun::id);
        if (run == methodology_runs_.end() || !methodology_run_credential_live(*run) ||
            !constant_time_hex_equal(identity_crypto::sha256_hex(secret), run->credential_sha256)) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        return methodology_run_principal(*run, identity_crypto::sha256_hex(secret));
    }
    if (secret.starts_with(agent_launch_credential_prefix)) {
        const auto parsed = parse_agent_launch_credential(secret);
        if (!parsed) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto launch = std::ranges::find(agent_launches_, parsed->first, &AgentLaunch::id);
        if (launch == agent_launches_.end() || launch->desired != "present" ||
            launch->credential_sha256.empty() ||
            !constant_time_hex_equal(identity_crypto::sha256_hex(secret),
                                     launch->credential_sha256)) {
            return std::unexpected(StoreError{"not_found", "session is not active"});
        }
        const auto user = std::ranges::find_if(
            users_, [&](const User& row) { return row.id == launch->created_by_user_id; });
        if (user == users_.end() || user->taap_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "launch identity is unavailable"});
        }
        return Principal{user->id,
                         user->taap_id,
                         "launch:" + launch->id,
                         "",
                         "lake",
                         "agent_launch",
                         launch->scopes,
                         std::nullopt};
    }
    for (auto& session : sessions_) {
        if (!session.revoked_at.empty() ||
            !identity_crypto::verify_secret(session_hash(session), secret))
            continue;
        // Told apart from an unknown secret only once the secret matched (lake#450).
        if (!session.expires_at.empty() &&
            std::stoll(session.expires_at) <=
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()) {
            return std::unexpected(StoreError{"expired", "session has expired"});
        }
        if (!active_session_lineage(sessions_, devices_, session)) {
            continue;
        }
        const auto user = std::ranges::find_if(
            users_, [&](const User& row) { return row.id == session.user_id; });
        if (user == users_.end() || user->taap_id.empty())
            return std::unexpected(StoreError{"unauthorized", "session identity is unavailable"});
        session.last_used_at = now_iso8601();
        return Principal{session.user_id,
                         user->taap_id,
                         session.id,
                         session.device_id,
                         session.pool_id,
                         session.kind,
                         session.scopes,
                         session.expires_at.empty()
                             ? std::nullopt
                             : std::optional<std::int64_t>(std::stoll(session.expires_at))};
    }
    return std::unexpected(StoreError{"unauthorized", "invalid, expired, or revoked credential"});
}

Result<SsoProvider> InMemoryStore::register_sso_provider(const NewSsoProvider& input) {
    if (input.organization_id.empty() || input.name.empty() || input.issuer.empty() ||
        input.scim_token.empty() || (input.protocol != "saml" && input.protocol != "oidc")) {
        return std::unexpected(StoreError{"invalid_input", "provider configuration is incomplete"});
    }
    if ((input.protocol == "saml" &&
         (input.metadata.empty() || input.signing_certificate.empty())) ||
        (input.protocol == "oidc" && (input.client_id.empty() || input.client_secret.empty()))) {
        return std::unexpected(
            StoreError{"invalid_input", "provider protocol configuration is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            organizations_.begin(), organizations_.end(), [&](const Organization& organization) {
                return organization.id == input.organization_id;
            })) {
        return std::unexpected(StoreError{"not_found", "organization does not exist"});
    }
    const std::string now = now_iso8601();
    for (SsoProvider& provider : sso_providers_) {
        if (provider.organization_id == input.organization_id && provider.name == input.name) {
            provider.pool_id = input.pool_id.empty() ? "lake" : input.pool_id;
            provider.protocol = input.protocol;
            provider.issuer = input.issuer;
            provider.metadata = input.metadata;
            provider.signing_certificate = input.signing_certificate;
            provider.client_id = input.client_id;
            provider.attribute_mapping = input.attribute_mapping;
            provider.jit_provisioning = input.jit_provisioning;
            provider.enabled = true;
            provider.updated_at = now;
            sso_scim_tokens_.erase(
                std::remove_if(sso_scim_tokens_.begin(),
                               sso_scim_tokens_.end(),
                               [&](const auto& entry) { return entry.first == provider.id; }),
                sso_scim_tokens_.end());
            const std::string token_hash = identity_crypto::hash_secret(input.scim_token);
            if (token_hash.empty()) {
                return std::unexpected(StoreError{"store_error", "could not hash SCIM token"});
            }
            sso_scim_tokens_.emplace_back(provider.id, token_hash);
            sso_oidc_secrets_.erase(
                std::remove_if(sso_oidc_secrets_.begin(),
                               sso_oidc_secrets_.end(),
                               [&](const auto& entry) { return entry.first == provider.id; }),
                sso_oidc_secrets_.end());
            if (input.protocol == "oidc") {
                sso_oidc_secrets_.emplace_back(provider.id, input.client_secret);
            }
            return provider;
        }
    }
    SsoProvider provider{new_uuid_v4(),
                         input.organization_id,
                         input.pool_id.empty() ? "lake" : input.pool_id,
                         input.name,
                         input.protocol,
                         input.issuer,
                         input.metadata,
                         input.signing_certificate,
                         input.client_id,
                         input.attribute_mapping,
                         input.jit_provisioning,
                         true,
                         "",
                         now,
                         now};
    const std::string token_hash = identity_crypto::hash_secret(input.scim_token);
    if (token_hash.empty()) {
        return std::unexpected(StoreError{"store_error", "could not hash SCIM token"});
    }
    sso_scim_tokens_.emplace_back(provider.id, token_hash);
    if (input.protocol == "oidc") {
        sso_oidc_secrets_.emplace_back(provider.id, input.client_secret);
    }
    sso_providers_.push_back(provider);
    return provider;
}

Result<std::vector<SsoProvider>>
InMemoryStore::list_sso_providers(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<SsoProvider> result;
    for (auto iterator = sso_providers_.rbegin(); iterator != sso_providers_.rend(); ++iterator) {
        if (organization_id.empty() || iterator->organization_id == organization_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<SsoProvider> InMemoryStore::get_sso_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider = std::find_if(sso_providers_.begin(),
                                       sso_providers_.end(),
                                       [&](const SsoProvider& item) { return item.id == id; });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    return *provider;
}

Result<OAuthProvider> InMemoryStore::get_sso_oidc_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == id && item.protocol == "oidc" && item.enabled;
        });
    const auto secret = std::find_if(sso_oidc_secrets_.begin(),
                                     sso_oidc_secrets_.end(),
                                     [&](const auto& item) { return item.first == id; });
    if (provider == sso_providers_.end() || secret == sso_oidc_secrets_.end()) {
        return std::unexpected(StoreError{"not_found", "OIDC provider is unavailable"});
    }
    return OAuthProvider{provider->pool_id,
                         "sso-" + provider->id,
                         provider->issuer,
                         provider->client_id,
                         secret->second,
                         {"openid", "email", "profile"}};
}

Result<SsoProvider> InMemoryStore::authenticate_scim(const std::string& bearer_token) {
    std::lock_guard lock(mutex_);
    for (const auto& [provider_id, token_hash] : sso_scim_tokens_) {
        if (!identity_crypto::verify_secret(token_hash, bearer_token)) {
            continue;
        }
        const auto provider = std::ranges::find_if(sso_providers_, [&](const SsoProvider& item) {
            return item.id == provider_id && item.enabled;
        });
        if (provider != sso_providers_.end()) {
            return *provider;
        }
    }
    return std::unexpected(StoreError{"unauthorized", "SCIM bearer token is invalid"});
}

Result<SsoProvider> InMemoryStore::test_sso_provider(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto provider = std::find_if(sso_providers_.begin(),
                                       sso_providers_.end(),
                                       [&](const SsoProvider& item) { return item.id == id; });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    if ((provider->protocol == "saml" &&
         (provider->metadata.empty() || provider->signing_certificate.empty())) ||
        (provider->protocol == "oidc" && provider->client_id.empty())) {
        return std::unexpected(StoreError{"invalid_input", "SSO provider is not testable"});
    }
    provider->tested_at = now_iso8601();
    provider->updated_at = provider->tested_at;
    return *provider;
}

Result<FederationIssuer> InMemoryStore::create_federation_issuer(const NewFederationIssuer& input) {
    if (input.organization_id.empty() || input.issuer.empty() || input.claim.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "organization, issuer, and claim are required"});
    }
    std::lock_guard lock(mutex_);
    for (const FederationIssuer& item : federation_issuers_) {
        if (item.organization_id == input.organization_id && item.issuer == input.issuer &&
            item.claim == input.claim) {
            return std::unexpected(
                StoreError{"conflict", "federation issuer claim already exists"});
        }
    }
    FederationIssuer issuer{
        new_uuid_v4(), input.organization_id, input.issuer, input.claim, now_iso8601()};
    federation_issuers_.push_back(issuer);
    return issuer;
}

Result<std::vector<FederationIssuer>>
InMemoryStore::list_federation_issuers(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<FederationIssuer> result;
    for (auto iterator = federation_issuers_.rbegin(); iterator != federation_issuers_.rend();
         ++iterator) {
        if (organization_id.empty() || iterator->organization_id == organization_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<bool> InMemoryStore::allows_federation_claim(const std::string& organization_id,
                                                    const std::string& issuer,
                                                    const std::string& claim) {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(federation_issuers_, [&](const FederationIssuer& value) {
        return value.organization_id == organization_id && value.issuer == issuer &&
               value.claim == claim;
    });
}

Result<IssuedSession> InMemoryStore::sign_in_sso(const SsoIdentity& input) {
    if (input.provider_id.empty() || input.issuer.empty() || input.subject.empty() ||
        !input.claims.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "SSO assertion is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == input.provider_id;
        });
    if (provider == sso_providers_.end() || !provider->enabled ||
        provider->issuer != input.issuer) {
        return std::unexpected(StoreError{"unauthorized", "SSO issuer is not accepted"});
    }
    for (auto claim = input.claims.begin(); claim != input.claims.end(); ++claim) {
        const bool allowed =
            std::any_of(federation_issuers_.begin(),
                        federation_issuers_.end(),
                        [&](const FederationIssuer& rule) {
                            return rule.organization_id == provider->organization_id &&
                                   rule.issuer == input.issuer && rule.claim == claim.key();
                        });
        if (!allowed) {
            return std::unexpected(
                StoreError{"unauthorized", "issuer is not allowed to assert " + claim.key()});
        }
    }
    auto scim = std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& user) {
        return user.provider_id == input.provider_id && user.external_id == input.subject;
    });
    std::string user_id;
    if (scim != scim_users_.end()) {
        if (!scim->active) {
            return std::unexpected(StoreError{"unauthorized", "provisioned user is inactive"});
        }
        user_id = scim->user_id;
    } else {
        if (!provider->jit_provisioning || input.taap_id.empty()) {
            return std::unexpected(StoreError{"unauthorized", "SSO user is not provisioned"});
        }
        const std::string now = now_iso8601();
        users_.push_back(
            User{new_uuid_v4(), input.taap_id, input.email, input.display_name, now, now});
        user_id = users_.back().id;
        scim_users_.push_back(
            ScimUser{input.provider_id, input.subject, user_id, true, input.claims, now, now});
    }
    const auto user = std::find_if(
        users_.begin(), users_.end(), [&](const User& item) { return item.id == user_id; });
    if (user == users_.end() || user->taap_id.empty()) {
        return std::unexpected(StoreError{"unauthorized", "SSO user has no TAAP identity"});
    }
    const auto credential = std::find_if(
        sso_credentials_.begin(), sso_credentials_.end(), [&](const SsoCredential& item) {
            return item.provider_id == input.provider_id && item.subject == input.subject;
        });
    if (credential == sso_credentials_.end()) {
        sso_credentials_.push_back(SsoCredential{input.provider_id, input.subject, user_id});
    }
    Device& device = add_device(devices_, user_id, input.device_name);
    return add_session(
        sessions_, user_id, provider->pool_id, device.id, "browser", "", {"*"}, "", 86400);
}

Result<std::vector<ScimUser>> InMemoryStore::list_scim_users(const std::string& provider_id,
                                                             int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ScimUser> result;
    for (const ScimUser& user : scim_users_) {
        if (user.provider_id == provider_id) {
            result.push_back(user);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<ScimUser> InMemoryStore::get_scim_user(const std::string& provider_id,
                                              const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto user = std::ranges::find_if(scim_users_, [&](const ScimUser& item) {
        return item.provider_id == provider_id &&
               (item.user_id == resource_id || item.external_id == resource_id);
    });
    if (user == scim_users_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
    }
    return *user;
}

void InMemoryStore::remove_scim_user_access(const ScimUser& user, const std::string& now) {
    for (Session& session : sessions_) {
        if (session.user_id == user.user_id && session.revoked_at.empty()) {
            session.revoked_at = now;
        }
    }
    std::vector<std::string> membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        if (member.provider_id == user.provider_id && member.user_external_id == user.external_id) {
            membership_ids.push_back(member.membership_id);
        }
    }
    for (const std::string& membership_id : membership_ids) {
        const auto membership = std::ranges::find_if(
            memberships_, [&](const Membership& item) { return item.id == membership_id; });
        if (membership == memberships_.end()) {
            continue;
        }
        const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
            return item.object_type == "team" && item.external_id == membership->team_id;
        });
        if (object != policy_objects_.end()) {
            grants_.erase(std::remove_if(grants_.begin(),
                                         grants_.end(),
                                         [&](const Grant& grant) {
                                             return grant.object_id == object->id &&
                                                    grant.principal_taap_id ==
                                                        membership->principal_taap_id &&
                                                    grant.attestation_digest ==
                                                        membership->attestation_digest;
                                         }),
                          grants_.end());
        }
        policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                             policy_objects_.end(),
                                             [&](const PolicyObject& item) {
                                                 return item.object_type == "member" &&
                                                        item.external_id == membership_id;
                                             }),
                              policy_objects_.end());
        memberships_.erase(membership);
    }
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return member.provider_id == user.provider_id &&
                                                        member.user_external_id == user.external_id;
                                             }),
                              scim_group_members_.end());
}

Result<ScimUser> InMemoryStore::upsert_scim_user(const NewScimUser& input) {
    if (input.provider_id.empty() || input.external_id.empty() || input.taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "provider, external id, and TAAP identity are required"});
    }
    std::lock_guard lock(mutex_);
    if (std::none_of(
            sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& provider) {
                return provider.id == input.provider_id;
            })) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    const std::string now = now_iso8601();
    const auto record =
        std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& user) {
            return user.provider_id == input.provider_id && user.external_id == input.external_id;
        });
    if (record != scim_users_.end()) {
        const auto user = std::find_if(
            users_.begin(), users_.end(), [&](User& item) { return item.id == record->user_id; });
        if (user != users_.end()) {
            user->email = input.email;
            user->display_name = input.display_name;
            user->updated_at = now;
        }
        record->active = input.active;
        record->attributes = input.attributes;
        record->updated_at = now;
        if (!input.active) {
            remove_scim_user_access(*record, now);
        }
        provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                     input.provider_id,
                                                     "user",
                                                     input.active ? "update" : "deactivate",
                                                     input.external_id,
                                                     "success",
                                                     {},
                                                     now});
        return *record;
    }
    users_.push_back(User{new_uuid_v4(), input.taap_id, input.email, input.display_name, now, now});
    ScimUser created{input.provider_id,
                     input.external_id,
                     users_.back().id,
                     input.active,
                     input.attributes,
                     now,
                     now};
    scim_users_.push_back(created);
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 input.provider_id,
                                                 "user",
                                                 input.active ? "create" : "deactivate",
                                                 input.external_id,
                                                 "success",
                                                 {},
                                                 now});
    return created;
}

Result<ScimUser> InMemoryStore::delete_scim_user(const std::string& provider_id,
                                                 const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto user = std::ranges::find_if(scim_users_, [&](const ScimUser& item) {
        return item.provider_id == provider_id &&
               (item.user_id == resource_id || item.external_id == resource_id);
    });
    if (user == scim_users_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM user does not exist"});
    }
    const std::string now = now_iso8601();
    user->active = false;
    user->updated_at = now;
    remove_scim_user_access(*user, now);
    provisioning_logs_.push_back(ProvisioningLog{
        new_uuid_v4(), provider_id, "user", "deactivate", user->external_id, "success", {}, now});
    return *user;
}

Result<std::vector<ScimGroup>> InMemoryStore::list_scim_groups(const std::string& provider_id,
                                                               int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ScimGroup> result;
    for (const ScimGroup& group : scim_groups_) {
        if (group.provider_id == provider_id) {
            result.push_back(group);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

Result<ScimGroup> InMemoryStore::get_scim_group(const std::string& provider_id,
                                                const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto group = std::ranges::find_if(scim_groups_, [&](const ScimGroup& item) {
        return item.provider_id == provider_id &&
               (item.team_id == resource_id || item.external_id == resource_id);
    });
    if (group == scim_groups_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
    }
    return *group;
}

Result<ScimGroup> InMemoryStore::upsert_scim_group(const NewScimGroup& input) {
    if (input.provider_id.empty() || input.external_id.empty() || input.team_name.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "provider, external id, and team name are required"});
    }
    std::lock_guard lock(mutex_);
    const auto provider =
        std::find_if(sso_providers_.begin(), sso_providers_.end(), [&](const SsoProvider& item) {
            return item.id == input.provider_id;
        });
    if (provider == sso_providers_.end()) {
        return std::unexpected(StoreError{"not_found", "SSO provider does not exist"});
    }
    const std::string now = now_iso8601();
    auto group = std::find_if(scim_groups_.begin(), scim_groups_.end(), [&](const ScimGroup& item) {
        return item.provider_id == input.provider_id && item.external_id == input.external_id;
    });
    const bool creating = group == scim_groups_.end();
    if (group == scim_groups_.end()) {
        if (!valid_handle(input.team_name)) {
            return std::unexpected(StoreError{"invalid_input", "team name is invalid"});
        }
        teams_.push_back(Team{new_uuid_v4(),
                              provider->organization_id,
                              input.team_name,
                              input.display_name,
                              now,
                              now});
        const auto parent = std::find_if(
            policy_objects_.begin(), policy_objects_.end(), [&](const PolicyObject& item) {
                return item.object_type == "organization" &&
                       item.external_id == provider->organization_id;
            });
        if (parent == policy_objects_.end()) {
            return std::unexpected(
                StoreError{"store_error", "organization policy object is missing"});
        }
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "team", teams_.back().id, parent->id, now, now, true});
        scim_groups_.push_back(ScimGroup{input.provider_id,
                                         input.external_id,
                                         teams_.back().id,
                                         input.display_name,
                                         input.attributes,
                                         now,
                                         now});
        group = std::prev(scim_groups_.end());
    } else {
        group->display_name = input.display_name;
        group->attributes = input.attributes;
        group->updated_at = now;
    }
    const auto team_object =
        std::find_if(policy_objects_.begin(), policy_objects_.end(), [&](const PolicyObject& item) {
            return item.object_type == "team" && item.external_id == group->team_id;
        });
    if (team_object == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "team policy object is missing"});
    }
    std::vector<std::string> removed_membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        const bool belongs_to_group = member.provider_id == input.provider_id &&
                                      member.group_external_id == input.external_id;
        const bool is_still_member =
            std::ranges::find(input.member_external_ids, member.user_external_id) !=
            input.member_external_ids.end();
        if (belongs_to_group && !is_still_member) {
            removed_membership_ids.push_back(member.membership_id);
        }
    }
    for (const std::string& membership_id : removed_membership_ids) {
        const auto membership =
            std::find_if(memberships_.begin(), memberships_.end(), [&](const Membership& item) {
                return item.id == membership_id;
            });
        if (membership == memberships_.end()) {
            continue;
        }
        grants_.erase(std::remove_if(grants_.begin(),
                                     grants_.end(),
                                     [&](const Grant& grant) {
                                         return grant.object_id == team_object->id &&
                                                grant.principal_taap_id ==
                                                    membership->principal_taap_id;
                                     }),
                      grants_.end());
        policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                             policy_objects_.end(),
                                             [&](const PolicyObject& item) {
                                                 return item.object_type == "member" &&
                                                        item.external_id == membership_id;
                                             }),
                              policy_objects_.end());
        memberships_.erase(membership);
    }
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return std::ranges::find(removed_membership_ids,
                                                                          member.membership_id) !=
                                                        removed_membership_ids.end();
                                             }),
                              scim_group_members_.end());
    for (const std::string& external_id : input.member_external_ids) {
        const auto user =
            std::find_if(scim_users_.begin(), scim_users_.end(), [&](const ScimUser& item) {
                return item.provider_id == input.provider_id && item.external_id == external_id &&
                       item.active;
            });
        if (user == scim_users_.end()) {
            return std::unexpected(StoreError{"invalid_input", "SCIM group member is not active"});
        }
        const bool mapped = std::any_of(scim_group_members_.begin(),
                                        scim_group_members_.end(),
                                        [&](const ScimGroupMember& member) {
                                            return member.provider_id == input.provider_id &&
                                                   member.group_external_id == input.external_id &&
                                                   member.user_external_id == external_id;
                                        });
        if (mapped) {
            continue;
        }
        const auto lake_user = std::find_if(users_.begin(), users_.end(), [&](const User& item) {
            return item.id == user->user_id;
        });
        if (lake_user == users_.end() || lake_user->taap_id.empty()) {
            return std::unexpected(StoreError{"invalid_input", "SCIM user has no TAAP identity"});
        }
        Membership membership{new_uuid_v4(),
                              provider->organization_id,
                              group->team_id,
                              "user",
                              lake_user->id,
                              lake_user->taap_id,
                              "member",
                              "scim:" + input.provider_id + ":" + input.external_id,
                              provider->issuer,
                              now,
                              now};
        memberships_.push_back(membership);
        policy_objects_.push_back(
            PolicyObject{new_uuid_v4(), "member", membership.id, team_object->id, now, now, true});
        grants_.push_back(Grant{new_uuid_v4(),
                                lake_user->taap_id,
                                team_object->id,
                                "read",
                                membership.attestation_digest,
                                membership.granted_by_taap_id,
                                now,
                                now});
        scim_group_members_.push_back(
            ScimGroupMember{input.provider_id, input.external_id, external_id, membership.id});
    }
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 input.provider_id,
                                                 "group",
                                                 creating ? "create" : "update",
                                                 input.external_id,
                                                 "success",
                                                 {},
                                                 now});
    return *group;
}

Result<void> InMemoryStore::delete_scim_group(const std::string& provider_id,
                                              const std::string& resource_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(scim_groups_, [&](const ScimGroup& item) {
        return item.provider_id == provider_id &&
               (item.team_id == resource_id || item.external_id == resource_id);
    });
    if (found == scim_groups_.end()) {
        return std::unexpected(StoreError{"not_found", "SCIM group does not exist"});
    }
    const ScimGroup group = *found;
    const auto team_object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == "team" && item.external_id == group.team_id;
    });
    if (team_object != policy_objects_.end()) {
        const std::string object_id = team_object->id;
        grants_.erase(std::remove_if(grants_.begin(),
                                     grants_.end(),
                                     [&](const Grant& grant) {
                                         return grant.object_id == object_id &&
                                                grant.attestation_digest ==
                                                    "scim:" + provider_id + ":" + group.external_id;
                                     }),
                      grants_.end());
        policy_objects_.erase(team_object);
    }
    std::vector<std::string> membership_ids;
    for (const ScimGroupMember& member : scim_group_members_) {
        if (member.provider_id == provider_id && member.group_external_id == group.external_id) {
            membership_ids.push_back(member.membership_id);
        }
    }
    policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                         policy_objects_.end(),
                                         [&](const PolicyObject& object) {
                                             return object.object_type == "member" &&
                                                    std::ranges::find(membership_ids,
                                                                      object.external_id) !=
                                                        membership_ids.end();
                                         }),
                          policy_objects_.end());
    memberships_.erase(std::remove_if(memberships_.begin(),
                                      memberships_.end(),
                                      [&](const Membership& membership) {
                                          return std::ranges::find(membership_ids, membership.id) !=
                                                 membership_ids.end();
                                      }),
                       memberships_.end());
    scim_group_members_.erase(std::remove_if(scim_group_members_.begin(),
                                             scim_group_members_.end(),
                                             [&](const ScimGroupMember& member) {
                                                 return member.provider_id == provider_id &&
                                                        member.group_external_id ==
                                                            group.external_id;
                                             }),
                              scim_group_members_.end());
    teams_.erase(std::remove_if(teams_.begin(),
                                teams_.end(),
                                [&](const Team& team) { return team.id == group.team_id; }),
                 teams_.end());
    scim_groups_.erase(found);
    provisioning_logs_.push_back(ProvisioningLog{new_uuid_v4(),
                                                 provider_id,
                                                 "group",
                                                 "deactivate",
                                                 group.external_id,
                                                 "success",
                                                 {},
                                                 now_iso8601()});
    return {};
}

Result<std::vector<ProvisioningLog>>
InMemoryStore::list_sso_provisioning_logs(const std::string& provider_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ProvisioningLog> result;
    for (auto iterator = provisioning_logs_.rbegin(); iterator != provisioning_logs_.rend();
         ++iterator) {
        if (provider_id.empty() || iterator->provider_id == provider_id) {
            result.push_back(*iterator);
        }
        if (limit > 0 && result.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return result;
}

} // namespace lake::control

namespace lake::control {

Result<Repository> InMemoryStore::create_repository(const NewRepository& in) {
    std::lock_guard lock(mutex_);
    if (std::none_of(customers_.begin(), customers_.end(), [&](const Customer& c) {
            return c.id == in.customer_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "customer_id does not reference a customer"});
    }
    if (!valid_handle(in.name))
        return std::unexpected(
            StoreError{"invalid_input", "name must match repository_name_format"});
    const std::string visibility = in.visibility.empty() ? "private" : in.visibility;
    if (visibility != "private" && visibility != "organization" && visibility != "public") {
        return std::unexpected(StoreError{"invalid_input", "visibility must be private or public"});
    }
    if (!in.description.empty()) {
        return std::unexpected(StoreError{
            "invalid_input",
            "a repository's description is its settings record in its workspace (lake#315)"});
    }
    if (!in.id.empty() && std::ranges::any_of(repositories_, [&](const Repository& repository) {
            return repository.id == in.id;
        })) {
        return std::unexpected(StoreError{"conflict", "repository id " + in.id + " is taken"});
    }
    // As migration 0051's routing trigger: a live workspace of the repository's own space.
    if (std::ranges::none_of(workspaces_, [&](const Workspace& workspace) {
            return workspace.id == in.workspace_id && workspace.customer_id == in.customer_id &&
                   workspace.desired != "deleted";
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "workspace_id must name a workspace in this account"});
    }
    for (const auto& r : repositories_) {
        if (r.customer_id == in.customer_id && r.name == in.name && r.deleted_at.empty()) {
            return std::unexpected(
                StoreError{"conflict", "this customer already has a repository named " + in.name});
        }
    }
    const std::string now = now_iso8601();
    repositories_.push_back(Repository{in.id.empty() ? new_uuid_v4() : in.id,
                                       in.customer_id,
                                       in.name,
                                       "",
                                       visibility,
                                       "",
                                       "",
                                       now,
                                       now,
                                       in.workspace_id});
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        return object.object_type == "customer" && object.external_id == in.customer_id;
    });
    if (parent == policy_objects_.end()) {
        repositories_.pop_back();
        return std::unexpected(StoreError{"store_error", "customer policy object is missing"});
    }
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "repository", repositories_.back().id, parent->id, now, now, true});
    return repositories_.back();
}

Result<std::vector<Repository>> InMemoryStore::list_repositories(const std::string& customer_id,
                                                                 int limit,
                                                                 const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<Repository> out;
    for (auto it = repositories_.rbegin(); it != repositories_.rend(); ++it) {
        if ((customer_id.empty() || it->customer_id == customer_id) &&
            (workspace_id.empty() || it->workspace_id == workspace_id))
            out.push_back(*it);
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit))
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

Result<Repository> InMemoryStore::get_repository(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& r : repositories_)
        if (r.id == id)
            return r;
    return std::unexpected(StoreError{"not_found", "no repository " + id});
}

namespace {

std::vector<Repository>::iterator repository_by_id(std::vector<Repository>& repositories,
                                                   const std::string& id) {
    return std::find_if(repositories.begin(),
                        repositories.end(),
                        [&](const Repository& repository) { return repository.id == id; });
}

bool repository_name_taken(const std::vector<Repository>& repositories,
                           const std::string& customer_id,
                           const std::string& name,
                           const std::string& except_id) {
    return std::any_of(repositories.begin(), repositories.end(), [&](const Repository& repository) {
        return repository.id != except_id && repository.customer_id == customer_id &&
               repository.name == name && repository.deleted_at.empty();
    });
}

} // namespace

Result<Repository> InMemoryStore::rename_repository(const std::string& id,
                                                    const std::string& name) {
    std::lock_guard lock(mutex_);
    if (!valid_handle(name)) {
        return std::unexpected(
            StoreError{"invalid_input", "name must match repository_name_format"});
    }
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    if (repository_name_taken(repositories_, repository->customer_id, name, id)) {
        return std::unexpected(
            StoreError{"conflict", "this customer already has a repository named " + name});
    }
    repository->name = name;
    repository->updated_at = now_iso8601();
    return *repository;
}

Result<Repository> InMemoryStore::set_repository_visibility(const std::string& id,
                                                            const std::string& visibility) {
    std::lock_guard lock(mutex_);
    if (visibility != "private" && visibility != "organization" && visibility != "public") {
        return std::unexpected(
            StoreError{"invalid_input", "visibility must be private, organization, or public"});
    }
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
    }
    repository->visibility = visibility;
    repository->updated_at = now_iso8601();
    return *repository;
}

Result<Repository> InMemoryStore::delete_repository(const std::string& id,
                                                    const RepositoryDeletion& deletion) {
    std::lock_guard lock(mutex_);
    const auto repository = repository_by_id(repositories_, id);
    if (repository == repositories_.end()) {
        return std::unexpected(StoreError{"not_found", "no repository " + id});
    }
    if (!repository->deleted_at.empty()) {
        return std::unexpected(StoreError{"conflict", "repository is already tombstoned"});
    }
    repository->deleted_at = deletion.deleted_at;
    repository->retention_until = deletion.retention_until;
    repository->updated_at = deletion.deleted_at;
    return *repository;
}

// Migration 0054's assignment: a row belongs to its repository's workspace. A row whose repository
// has no workspace is needs_selection until it has one; a row whose workspace was deleted went
// with it (workspace_deleted).
// Migration 0063's assignment, over the events this store holds (lake#317).
Result<void>
InMemoryStore::assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) {
    std::lock_guard lock(mutex_);
    const auto uuid_after = [](const std::string& object, std::string_view prefix) {
        if (!object.starts_with(prefix) || object.size() < prefix.size() + 36) {
            return std::string();
        }
        return object.substr(prefix.size(), 36);
    };
    const auto repository_workspace = [this](const std::string& id) {
        const auto repository = repository_by_id(repositories_, id);
        return repository == repositories_.end() ? std::string() : repository->workspace_id;
    };
    for (const audit::Event& event : audit_events_) {
        if (legacy_audit_assignments_.contains(event.id) ||
            std::ranges::find(workspace_actions, event.draft.action) == workspace_actions.end() ||
            (event.draft.outcome == "denied" &&
             !event.draft.kind.starts_with("forge.branch_protection."))) {
            continue;
        }
        const nlohmann::json& details = event.draft.details;
        const auto detail = [&details](const char* key) {
            return details.is_object() && details.contains(key) && details.at(key).is_string()
                       ? details.at(key).get<std::string>()
                       : std::string();
        };
        std::string workspace_id = detail("workspace_id");
        if (workspace_id.empty()) {
            workspace_id =
                repository_workspace(uuid_after(event.draft.object, "repository/"));
        }
        if (workspace_id.empty()) {
            const std::string named = detail("repository_id");
            workspace_id = repository_workspace(named.empty() ? detail("repository") : named);
        }
        if (workspace_id.empty()) {
            const std::string app = uuid_after(event.draft.object, "app/");
            const auto found = std::ranges::find(web_apps_, app, &WebApp::id);
            workspace_id = !app.empty() && found != web_apps_.end() ? found->workspace_id : "";
        }
        if (workspace_id.empty()) {
            const std::string build = uuid_after(event.draft.object, "build/");
            const auto found = std::ranges::find(builds_, build, &Build::id);
            workspace_id = !build.empty() && found != builds_.end() ? found->workspace_id : "";
        }
        if (workspace_id.empty()) {
            const std::string mount = uuid_after(event.draft.object, "secret_mount/");
            const auto found =
                std::ranges::find(secret_mount_routes_, mount, &SecretMountRoute::mount_id);
            workspace_id =
                !mount.empty() && found != secret_mount_routes_.end() ? found->workspace_id : "";
        }
        if (workspace_id.empty()) {
            const std::string task = uuid_after(event.draft.object, "agent_task/");
            const auto found = std::ranges::find(agent_tasks_, task, &AgentTask::id);
            workspace_id = !task.empty() && found != agent_tasks_.end() ? found->workspace_id : "";
        }
        const auto workspace = std::ranges::find(workspaces_, workspace_id, &Workspace::id);
        if (workspace_id.empty() || workspace == workspaces_.end() ||
            (!event.draft.customer_id.empty() &&
             workspace->customer_id != event.draft.customer_id)) {
            continue; // stays central
        }
        legacy_audit_assignments_[event.id] =
            workspace->desired == "deleted"
                ? std::pair<std::string, std::string>{"", "workspace_deleted"}
                : std::pair<std::string, std::string>{workspace->id, "event_workspace"};
    }
    return {};
}

Result<std::vector<LegacyAuditEvent>>
InMemoryStore::legacy_audit_events(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<LegacyAuditEvent> rows;
    bool after = after_id.empty();
    for (const audit::Event& event : audit_events_) {
        if (!after) {
            after = event.id == after_id;
            continue;
        }
        const auto assigned = legacy_audit_assignments_.find(event.id);
        if (assigned == legacy_audit_assignments_.end()) {
            continue;
        }
        if (static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            break;
        }
        std::string customer_id = event.draft.customer_id;
        if (const auto workspace =
                std::ranges::find(workspaces_, assigned->second.first, &Workspace::id);
            workspace != workspaces_.end()) {
            customer_id = workspace->customer_id;
        }
        rows.push_back({event, customer_id, assigned->second.first, assigned->second.second});
    }
    return rows;
}

Result<DriveShareLinkRoute>
InMemoryStore::drive_share_link_route(const std::string& token_hash) {
    std::lock_guard lock(mutex_);
    const auto route = drive_share_link_routes_.find(token_hash);
    if (route == drive_share_link_routes_.end()) {
        return std::unexpected(StoreError{"not_found", "share link is not recognized"});
    }
    return route->second;
}

Result<Team> InMemoryStore::create_team(const NewTeam& in) {
    std::lock_guard lock(mutex_);
    if (!valid_handle(in.name)) {
        return std::unexpected(StoreError{"invalid_input", "name must match team_name_format"});
    }
    if (std::ranges::none_of(organizations_, [&](const Organization& organization) {
            return organization.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (std::ranges::any_of(teams_, [&](const Team& team) {
            return team.organization_id == in.organization_id && team.name == in.name;
        })) {
        return std::unexpected(
            StoreError{"conflict", "this organization already has a team named " + in.name});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        return object.object_type == "organization" && object.external_id == in.organization_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "organization policy object is missing"});
    }
    const std::string now = now_iso8601();
    teams_.push_back(Team{new_uuid_v4(), in.organization_id, in.name, in.display_name, now, now});
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "team", teams_.back().id, parent->id, now, now, true});
    return teams_.back();
}

Result<std::vector<Team>> InMemoryStore::list_teams(const std::string& organization_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Team> out;
    for (auto team = teams_.rbegin(); team != teams_.rend(); ++team) {
        if (organization_id.empty() || team->organization_id == organization_id) {
            out.push_back(*team);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Team> InMemoryStore::get_team(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& team : teams_) {
        if (team.id == id) {
            return team;
        }
    }
    return std::unexpected(StoreError{"not_found", "no team " + id});
}

void InMemoryStore::derive_space_grant_locked(const Membership& membership,
                                              const std::string& role) {
    if (!membership.team_id.empty()) {
        return;
    }
    const auto customer = std::ranges::find(
        customers_, membership.organization_id, &Customer::organization_id);
    if (customer == customers_.end()) {
        return;
    }
    const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == "customer" && item.external_id == customer->id;
    });
    if (object == policy_objects_.end()) {
        return;
    }
    const std::string grant_role = policy_role_for_membership(role);
    if (std::ranges::any_of(grants_, [&](const Grant& item) {
            return item.principal_taap_id == membership.principal_taap_id &&
                   item.object_id == object->id && item.role == grant_role;
        })) {
        return;
    }
    const std::string now = now_iso8601();
    grants_.push_back(Grant{new_uuid_v4(),
                            membership.principal_taap_id,
                            object->id,
                            grant_role,
                            membership.attestation_digest,
                            membership.granted_by_taap_id,
                            now,
                            now});
}

void InMemoryStore::drop_space_grant_locked(const Membership& membership,
                                            const std::string& role) {
    if (!membership.team_id.empty()) {
        return;
    }
    const auto customer = std::ranges::find(
        customers_, membership.organization_id, &Customer::organization_id);
    if (customer == customers_.end()) {
        return;
    }
    const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == "customer" && item.external_id == customer->id;
    });
    if (object == policy_objects_.end()) {
        return;
    }
    const std::string object_id = object->id;
    const std::string grant_role = policy_role_for_membership(role);
    std::erase_if(grants_, [&](const Grant& item) {
        return item.principal_taap_id == membership.principal_taap_id &&
               item.object_id == object_id && item.role == grant_role &&
               item.attestation_digest == membership.attestation_digest &&
               item.granted_by_taap_id == membership.granted_by_taap_id;
    });
}

Result<Membership> InMemoryStore::create_membership(const NewMembership& in) {
    std::lock_guard lock(mutex_);
    if ((in.principal_type != "user" && in.principal_type != "workload") ||
        in.principal_id.empty() || in.principal_taap_id.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "a user or workload principal and TAAP identity are required"});
    }
    if (!valid_membership_role(in.role) || in.attestation_digest.empty() ||
        in.granted_by_taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "role and attestation evidence are required"});
    }
    if (in.principal_type == "user") {
        const auto user = std::ranges::find_if(users_, [&](const User& row) {
            return row.id == in.principal_id && row.taap_id == in.principal_taap_id;
        });
        if (user == users_.end()) {
            return std::unexpected(
                StoreError{"invalid_input", "user principal does not match its TAAP identity"});
        }
    }
    const auto organization = std::ranges::find_if(
        organizations_, [&](const Organization& row) { return row.id == in.organization_id; });
    if (organization == organizations_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!in.team_id.empty()) {
        const auto team = std::ranges::find_if(teams_, [&](const Team& row) {
            return row.id == in.team_id && row.organization_id == in.organization_id;
        });
        if (team == teams_.end()) {
            return std::unexpected(
                StoreError{"invalid_input", "team_id does not belong to the organization"});
        }
    }
    if (std::ranges::any_of(memberships_, [&](const Membership& row) {
            return row.organization_id == in.organization_id && row.team_id == in.team_id &&
                   row.principal_type == in.principal_type && row.principal_id == in.principal_id;
        })) {
        return std::unexpected(
            StoreError{"conflict", "the principal is already a member of this scope"});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        const std::string type = in.team_id.empty() ? "organization" : "team";
        const std::string external_id = in.team_id.empty() ? in.organization_id : in.team_id;
        return object.object_type == type && object.external_id == external_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    const std::string parent_object_id = parent->id;
    const std::string now = now_iso8601();
    memberships_.push_back(Membership{new_uuid_v4(),
                                      in.organization_id,
                                      in.team_id,
                                      in.principal_type,
                                      in.principal_id,
                                      in.principal_taap_id,
                                      in.role,
                                      in.attestation_digest,
                                      in.granted_by_taap_id,
                                      now,
                                      now});
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", memberships_.back().id, parent_object_id, now, now, true});
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            parent_object_id,
                            policy_role_for_membership(in.role),
                            in.attestation_digest,
                            in.granted_by_taap_id,
                            now,
                            now});
    derive_space_grant_locked(memberships_.back(), in.role);
    return memberships_.back();
}

Result<std::vector<Membership>> InMemoryStore::list_memberships(const std::string& organization_id,
                                                                const std::string& team_id,
                                                                int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Membership> out;
    for (auto membership = memberships_.rbegin(); membership != memberships_.rend(); ++membership) {
        if ((organization_id.empty() || membership->organization_id == organization_id) &&
            (team_id.empty() || membership->team_id == team_id)) {
            out.push_back(*membership);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Membership> InMemoryStore::get_membership(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto membership = std::ranges::find(memberships_, id, &Membership::id);
    if (membership == memberships_.end()) {
        return std::unexpected(StoreError{"not_found", "no membership " + id});
    }
    return *membership;
}

Result<Membership> InMemoryStore::set_member_role(const std::string& id,
                                                  const std::string& role,
                                                  const std::string& actor) {
    if (!valid_membership_role(role)) {
        return std::unexpected(StoreError{"invalid_input", "role must be owner, admin, or member"});
    }
    std::lock_guard lock(mutex_);
    const auto membership = std::ranges::find(memberships_, id, &Membership::id);
    if (membership == memberships_.end()) {
        return std::unexpected(StoreError{"not_found", "no membership " + id});
    }
    if (membership->role == "owner" && role != "owner") {
        const auto owner_count = std::ranges::count_if(memberships_, [&](const Membership& item) {
            return item.organization_id == membership->organization_id &&
                   item.team_id == membership->team_id && item.role == "owner";
        });
        if (owner_count == 1) {
            return std::unexpected(
                StoreError{"conflict", "cannot demote the last owner in this membership scope"});
        }
    }
    const std::string previous_role = membership->role;
    membership->role = role;
    membership->updated_at = now_iso8601();
    const std::string object_type = membership->team_id.empty() ? "organization" : "team";
    const std::string external_id =
        membership->team_id.empty() ? membership->organization_id : membership->team_id;
    const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == object_type && item.external_id == external_id;
    });
    if (object == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    const auto grant = std::ranges::find_if(grants_, [&](const Grant& item) {
        return item.principal_taap_id == membership->principal_taap_id &&
               item.object_id == object->id &&
               item.role == policy_role_for_membership(previous_role) &&
               item.attestation_digest == membership->attestation_digest &&
               item.granted_by_taap_id == membership->granted_by_taap_id;
    });
    if (grant == grants_.end()) {
        return std::unexpected(StoreError{"store_error", "membership derived grant is missing"});
    }
    grant->role = policy_role_for_membership(role);
    grant->updated_at = membership->updated_at;
    Membership previous = *membership;
    previous.role = previous_role;
    drop_space_grant_locked(previous, previous_role);
    derive_space_grant_locked(*membership, role);
    static_cast<void>(actor);
    return *membership;
}

Result<void> InMemoryStore::remove_member(const std::string& id, const std::string& actor) {
    std::lock_guard lock(mutex_);
    const auto membership = std::ranges::find(memberships_, id, &Membership::id);
    if (membership == memberships_.end()) {
        return std::unexpected(StoreError{"not_found", "no membership " + id});
    }
    if (membership->role == "owner") {
        const auto owner_count = std::ranges::count_if(memberships_, [&](const Membership& item) {
            return item.organization_id == membership->organization_id &&
                   item.team_id == membership->team_id && item.role == "owner";
        });
        if (owner_count == 1) {
            const std::string message =
                membership->principal_taap_id == actor
                    ? "cannot remove yourself as the last owner in this membership scope"
                    : "cannot remove the last owner in this membership scope";
            return std::unexpected(StoreError{"conflict", message});
        }
    }
    const std::string object_type = membership->team_id.empty() ? "organization" : "team";
    const std::string external_id =
        membership->team_id.empty() ? membership->organization_id : membership->team_id;
    const auto object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& item) {
        return item.object_type == object_type && item.external_id == external_id;
    });
    if (object == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    grants_.erase(
        std::remove_if(grants_.begin(),
                       grants_.end(),
                       [&](const Grant& item) {
                           return item.principal_taap_id == membership->principal_taap_id &&
                                  item.object_id == object->id &&
                                  item.role == policy_role_for_membership(membership->role) &&
                                  item.attestation_digest == membership->attestation_digest &&
                                  item.granted_by_taap_id == membership->granted_by_taap_id;
                       }),
        grants_.end());
    drop_space_grant_locked(*membership, membership->role);
    policy_objects_.erase(std::remove_if(policy_objects_.begin(),
                                         policy_objects_.end(),
                                         [&](const PolicyObject& item) {
                                             return item.object_type == "member" &&
                                                    item.external_id == membership->id;
                                         }),
                          policy_objects_.end());
    memberships_.erase(membership);
    return {};
}

Result<Invitation> InMemoryStore::create_invitation(const NewInvitation& in) {
    std::lock_guard lock(mutex_);
    if ((in.invitee_type != "email" && in.invitee_type != "handle") || in.invitee.empty() ||
        !valid_membership_role(in.role) || in.expires_at.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "invitee, role, and expiry are required"});
    }
    if (in.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "expires_at must be in the future"});
    }
    if (std::ranges::none_of(organizations_, [&](const Organization& row) {
            return row.id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id does not reference an organization"});
    }
    if (!in.team_id.empty() && std::ranges::none_of(teams_, [&](const Team& row) {
            return row.id == in.team_id && row.organization_id == in.organization_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "team_id does not belong to the organization"});
    }
    const std::string now = now_iso8601();
    invitations_.push_back(Invitation{new_uuid_v4(),
                                      in.organization_id,
                                      in.team_id,
                                      in.invitee_type,
                                      lower(in.invitee),
                                      in.role,
                                      in.expires_at,
                                      "pending",
                                      {},
                                      in.created_by_taap_id,
                                      now,
                                      now});
    return invitations_.back();
}

Result<std::vector<Invitation>> InMemoryStore::list_invitations(const std::string& organization_id,
                                                                const std::string& team_id,
                                                                int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Invitation> out;
    for (auto invitation = invitations_.rbegin(); invitation != invitations_.rend(); ++invitation) {
        if ((organization_id.empty() || invitation->organization_id == organization_id) &&
            (team_id.empty() || invitation->team_id == team_id)) {
            out.push_back(*invitation);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<Invitation> InMemoryStore::get_invitation(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& invitation : invitations_) {
        if (invitation.id == id) {
            return invitation;
        }
    }
    return std::unexpected(StoreError{"not_found", "no invitation " + id});
}

Result<Membership> InMemoryStore::accept_invitation(const AcceptInvitation& in) {
    std::lock_guard lock(mutex_);
    const auto invitation = std::ranges::find_if(
        invitations_, [&](const Invitation& row) { return row.id == in.invitation_id; });
    if (invitation == invitations_.end()) {
        return std::unexpected(StoreError{"not_found", "no invitation " + in.invitation_id});
    }
    if (invitation->status != "pending") {
        return std::unexpected(StoreError{"conflict", "invitation is not pending"});
    }
    if (invitation->expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"conflict", "invitation has expired"});
    }
    if ((in.principal_type != "user" && in.principal_type != "workload") ||
        in.principal_id.empty() || in.principal_taap_id.empty() || in.attestation_digest.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "principal and attestation evidence are required"});
    }
    if (in.principal_type == "user" && std::ranges::none_of(users_, [&](const User& row) {
            return row.id == in.principal_id && row.taap_id == in.principal_taap_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "user principal does not match its TAAP identity"});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& object) {
        const std::string type = invitation->team_id.empty() ? "organization" : "team";
        const std::string external_id =
            invitation->team_id.empty() ? invitation->organization_id : invitation->team_id;
        return object.object_type == type && object.external_id == external_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "membership policy parent is missing"});
    }
    const std::string parent_object_id = parent->id;
    if (std::ranges::any_of(memberships_, [&](const Membership& row) {
            return row.organization_id == invitation->organization_id &&
                   row.team_id == invitation->team_id && row.principal_type == in.principal_type &&
                   row.principal_id == in.principal_id;
        })) {
        return std::unexpected(
            StoreError{"conflict", "the principal is already a member of this scope"});
    }
    const std::string now = now_iso8601();
    memberships_.push_back(Membership{new_uuid_v4(),
                                      invitation->organization_id,
                                      invitation->team_id,
                                      in.principal_type,
                                      in.principal_id,
                                      in.principal_taap_id,
                                      invitation->role,
                                      in.attestation_digest,
                                      in.accepted_by_taap_id,
                                      now,
                                      now});
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", memberships_.back().id, parent_object_id, now, now, true});
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            parent_object_id,
                            policy_role_for_membership(invitation->role),
                            in.attestation_digest,
                            in.accepted_by_taap_id,
                            now,
                            now});
    derive_space_grant_locked(memberships_.back(), invitation->role);
    invitation->status = "accepted";
    invitation->accepted_membership_id = memberships_.back().id;
    invitation->updated_at = now;
    return memberships_.back();
}

namespace {

bool invitation_addressed_to(const Invitation& invitation,
                             const std::string& email,
                             const std::vector<std::string>& handles) {
    if (invitation.invitee_type == "email") {
        return !email.empty() && invitation.invitee == lower(email);
    }
    return std::ranges::any_of(
        handles, [&](const std::string& handle) { return invitation.invitee == lower(handle); });
}

} // namespace

Result<OrganizationSpace> InMemoryStore::create_organization_space(const NewOrganizationSpace& in) {
    std::lock_guard lock(mutex_);
    if (in.owner_user_id.empty() || in.owner_taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "an organization is started by a signed-in person"});
    }
    if (std::ranges::none_of(users_, [&](const User& row) {
            return row.id == in.owner_user_id && row.taap_id == in.owner_taap_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "user principal does not match its TAAP identity"});
    }
    std::string handle = in.name;
    if (handle.empty()) {
        do {
            handle = generate_handle();
        } while (std::ranges::any_of(customers_,
                                     [&](const Customer& row) { return row.name == handle; }));
    } else if (!valid_handle(handle)) {
        return std::unexpected(StoreError{
            "invalid_input",
            "handle must be lowercase letters, digits and inner hyphens (customer_name_format)"});
    } else if (std::ranges::any_of(customers_,
                                   [&](const Customer& row) { return row.name == handle; })) {
        return std::unexpected(StoreError{"conflict", "name already taken (customer_name_unique)"});
    }
    const std::string now = now_iso8601();
    const std::string evidence = "create_organization_space/" + in.request_id;
    OrganizationSpace space;
    space.organization = Organization{new_uuid_v4(), "", in.display_name, now, now};
    space.customer =
        Customer{new_uuid_v4(), "organization", "", space.organization.id, handle, now, now};
    const PolicyObject customer_object{new_uuid_v4(), "customer", space.customer.id, "", now, now,
                                       true};
    const PolicyObject organization_object{new_uuid_v4(), "organization", space.organization.id,
                                           customer_object.id, now, now, true};
    space.owner = Membership{new_uuid_v4(),
                             space.organization.id,
                             "",
                             "user",
                             in.owner_user_id,
                             in.owner_taap_id,
                             "owner",
                             evidence,
                             in.owner_taap_id,
                             now,
                             now};
    organizations_.push_back(space.organization);
    customers_.push_back(space.customer);
    policy_objects_.push_back(customer_object);
    policy_objects_.push_back(organization_object);
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", space.owner.id, organization_object.id, now, now, true});
    memberships_.push_back(space.owner);
    grants_.push_back(Grant{new_uuid_v4(), in.owner_taap_id, organization_object.id, "admin",
                            evidence, in.owner_taap_id, now, now});
    derive_space_grant_locked(space.owner, "owner");
    return space;
}

Result<OrganizationSpace> InMemoryStore::convert_personal_space(const ConvertPersonalSpace& in) {
    std::lock_guard lock(mutex_);
    if (in.owner_user_id.empty() || in.owner_taap_id.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "a person's own account is converted by the person who owns it"});
    }
    if (std::ranges::none_of(users_, [&](const User& row) {
            return row.id == in.owner_user_id && row.taap_id == in.owner_taap_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "user principal does not match its TAAP identity"});
    }
    const auto customer = std::ranges::find(customers_, in.customer_id, &Customer::id);
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "no customer " + in.customer_id});
    }
    if (customer->customer_type != "individual") {
        return std::unexpected(
            StoreError{"conflict", "this account already belongs to an organization"});
    }
    if (customer->user_id != in.owner_user_id) {
        return std::unexpected(StoreError{
            "unauthorized", "only the person whose own account this is can convert it"});
    }
    const auto customer_object = std::ranges::find_if(policy_objects_, [&](const PolicyObject& row) {
        return row.object_type == "customer" && row.external_id == in.customer_id;
    });
    if (customer_object == policy_objects_.end()) {
        return std::unexpected(StoreError{"store_error", "the account's policy object is missing"});
    }
    const std::string customer_object_id = customer_object->id;
    std::string fresh_handle;
    do {
        fresh_handle = generate_handle();
    } while (std::ranges::any_of(customers_,
                                 [&](const Customer& row) { return row.name == fresh_handle; }));
    const std::string now = now_iso8601();
    const std::string evidence = "convert_personal_space/" + in.request_id;
    OrganizationSpace space;
    space.organization = Organization{new_uuid_v4(), "", in.display_name, now, now};
    customer->customer_type = "organization";
    customer->user_id.clear();
    customer->organization_id = space.organization.id;
    customer->updated_at = now;
    space.customer = *customer;
    const PolicyObject organization_object{new_uuid_v4(), "organization", space.organization.id,
                                           customer_object_id, now, now, true};
    space.owner = Membership{new_uuid_v4(),
                             space.organization.id,
                             "",
                             "user",
                             in.owner_user_id,
                             in.owner_taap_id,
                             "owner",
                             evidence,
                             in.owner_taap_id,
                             now,
                             now};
    space.personal =
        Customer{new_uuid_v4(), "individual", in.owner_user_id, "", fresh_handle, now, now};
    const PolicyObject personal_object{new_uuid_v4(), "customer", space.personal.id, "", now, now,
                                       true};
    organizations_.push_back(space.organization);
    policy_objects_.push_back(organization_object);
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "member", space.owner.id, organization_object.id, now, now, true});
    memberships_.push_back(space.owner);
    grants_.push_back(Grant{new_uuid_v4(), in.owner_taap_id, organization_object.id, "admin",
                            evidence, in.owner_taap_id, now, now});
    derive_space_grant_locked(space.owner, "owner");
    customers_.push_back(space.personal);
    policy_objects_.push_back(personal_object);
    grants_.push_back(Grant{new_uuid_v4(), in.owner_taap_id, personal_object.id, "admin", evidence,
                            in.owner_taap_id, now, now});
    return space;
}

Result<std::vector<Membership>> InMemoryStore::list_user_memberships(const std::string& user_id,
                                                                     int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Membership> out;
    for (auto membership = memberships_.rbegin(); membership != memberships_.rend(); ++membership) {
        if (membership->principal_type == "user" && membership->principal_id == user_id) {
            out.push_back(*membership);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<std::vector<Invitation>>
InMemoryStore::list_invitee_invitations(const std::string& email,
                                        const std::vector<std::string>& handles,
                                        int limit) {
    std::lock_guard lock(mutex_);
    const std::string now = now_iso8601();
    std::vector<Invitation> out;
    for (auto invitation = invitations_.rbegin(); invitation != invitations_.rend(); ++invitation) {
        if (invitation->status == "pending" && invitation->expires_at > now &&
            invitation_addressed_to(*invitation, email, handles)) {
            out.push_back(*invitation);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<PolicyObject> InMemoryStore::create_policy_object(const NewPolicyObject& in) {
    std::lock_guard lock(mutex_);
    if (!valid_object_type(in.object_type) || in.external_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "valid object_type and external_id are required"});
    }
    if (in.parent_id.empty() && in.object_type != "customer") {
        return std::unexpected(
            StoreError{"invalid_input", "only a customer policy object may have no parent"});
    }
    if (std::ranges::any_of(policy_objects_, [&](const PolicyObject& object) {
            return object.object_type == in.object_type && object.external_id == in.external_id;
        })) {
        return std::unexpected(StoreError{"conflict", "policy object already exists"});
    }
    if (!in.parent_id.empty() &&
        std::ranges::none_of(policy_objects_, [&](const PolicyObject& object) {
            return object.id == in.parent_id;
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "parent_id does not reference a policy object"});
    }
    if (!in.parent_id.empty()) {
        const auto parent = std::ranges::find_if(
            policy_objects_, [&](const PolicyObject& object) { return object.id == in.parent_id; });
        if (!object_parent_allowed(in.object_type, parent->object_type)) {
            return std::unexpected(
                StoreError{"invalid_input", "object type is not valid under this parent"});
        }
    }
    const std::string now = now_iso8601();
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), in.object_type, in.external_id, in.parent_id, now, now, in.inherit});
    return policy_objects_.back();
}

Result<PolicyObject> InMemoryStore::get_policy_object(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (const auto& object : policy_objects_) {
        if (object.id == id) {
            return object;
        }
    }
    return std::unexpected(StoreError{"not_found", "no policy object " + id});
}

Result<PolicyObject> InMemoryStore::find_policy_object(const std::string& object_type,
                                                       const std::string& external_id) {
    std::lock_guard lock(mutex_);
    for (const auto& object : policy_objects_) {
        if (object.object_type == object_type && object.external_id == external_id) {
            return object;
        }
    }
    return std::unexpected(StoreError{"not_found", "no " + object_type + " object " + external_id});
}

Result<std::vector<PolicyObject>> InMemoryStore::list_policy_objects(const std::string& parent_id,
                                                                     const std::string& object_type,
                                                                     int limit) {
    std::lock_guard lock(mutex_);
    std::vector<PolicyObject> out;
    for (auto object = policy_objects_.rbegin(); object != policy_objects_.rend(); ++object) {
        if ((parent_id.empty() || object->parent_id == parent_id) &&
            (object_type.empty() || object->object_type == object_type)) {
            out.push_back(*object);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<std::vector<ObjectPathEntry>>
InMemoryStore::policy_paths(const std::vector<std::string>& requested_object_ids) {
    std::lock_guard lock(mutex_);
    std::vector<ObjectPathEntry> out;
    for (const auto& requested_id : requested_object_ids) {
        std::string current_id = requested_id;
        std::set<std::string> visited;
        int depth = 0;
        while (!current_id.empty() && !visited.contains(current_id)) {
            visited.insert(current_id);
            const auto object = std::ranges::find_if(
                policy_objects_, [&](const PolicyObject& row) { return row.id == current_id; });
            if (object == policy_objects_.end()) {
                break;
            }
            out.push_back(ObjectPathEntry{requested_id, *object, depth});
            if (!object->inherit) {
                break;
            }
            current_id = object->parent_id;
            ++depth;
        }
    }
    return out;
}

Result<Grant> InMemoryStore::create_grant(const NewGrant& in) {
    std::lock_guard lock(mutex_);
    // The messages name PostgreSQL's constraints, which ApiResult::failure says as rules.
    if (in.principal_taap_id.empty() || in.granted_by_taap_id.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "a principal and a granting identity are required"});
    }
    if (!valid_policy_role(in.role)) {
        return std::unexpected(StoreError{"invalid_input", "grants_role_check"});
    }
    if (in.attestation_digest.empty()) {
        return std::unexpected(StoreError{"invalid_input", "grants_attestation_check"});
    }
    const auto object = std::ranges::find_if(
        policy_objects_, [&](const PolicyObject& row) { return row.id == in.object_id; });
    if (object == policy_objects_.end()) {
        return std::unexpected(
            StoreError{"invalid_input", "object_id does not reference a policy object"});
    }
    if (!role_valid_for_object_type(object->object_type, in.role)) {
        return std::unexpected(
            StoreError{"invalid_input", "role is not valid for this object type"});
    }
    if (std::ranges::any_of(grants_, [&](const Grant& grant) {
            return grant.principal_taap_id == in.principal_taap_id &&
                   grant.object_id == in.object_id && grant.role == in.role;
        })) {
        return std::unexpected(
            StoreError{"conflict", "grant already exists (grants_principal_object_role_unique)"});
    }
    const std::string now = now_iso8601();
    grants_.push_back(Grant{new_uuid_v4(),
                            in.principal_taap_id,
                            in.object_id,
                            in.role,
                            in.attestation_digest,
                            in.granted_by_taap_id,
                            now,
                            now});
    return grants_.back();
}

Result<std::vector<Grant>>
InMemoryStore::list_grants(const std::vector<std::string>& object_ids,
                           const std::optional<std::string>& principal_taap_id,
                           int limit) {
    std::lock_guard lock(mutex_);
    std::set<std::string> wanted(object_ids.begin(), object_ids.end());
    std::vector<Grant> out;
    for (auto grant = grants_.rbegin(); grant != grants_.rend(); ++grant) {
        if ((wanted.empty() || wanted.contains(grant->object_id)) &&
            (!principal_taap_id || grant->principal_taap_id == *principal_taap_id)) {
            out.push_back(*grant);
        }
    }
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
        out.resize(static_cast<std::size_t>(limit));
    }
    return out;
}

Result<std::string> InMemoryStore::customer_kek_locked(const std::string& customer_id,
                                                       bool create) {
    auto record = std::ranges::find_if(customer_keks_, [&](const CustomerKekRecord& value) {
        return value.customer_id == customer_id;
    });
    if (record != customer_keks_.end()) {
        return record->key;
    }
    if (!create) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    auto generated = secrets_crypto::random_key();
    if (!generated) {
        return std::unexpected(generated.error());
    }
    std::string key(reinterpret_cast<const char*>(generated->data()), generated->size());
    secrets_crypto::cleanse(*generated);
    customer_keks_.push_back(CustomerKekRecord{customer_id, key, 1});
    return key;
}

Result<PkiAuthorityRecord> InMemoryStore::create_pki_authority(const NewPkiAuthorityRecord& input,
                                                               const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "no customer " + input.customer_id});
    }
    if (std::ranges::any_of(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == input.customer_id;
        })) {
        return std::unexpected(StoreError{"conflict", "customer authority already exists"});
    }
    auto raw_kek = customer_kek_locked(input.customer_id, true);
    if (!raw_kek || raw_kek->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_kek ? StoreError{"store_error", "customer key is invalid"}
                                       : raw_kek.error());
    }
    secrets_crypto::Key kek{};
    std::copy(raw_kek->begin(), raw_kek->end(), kek.begin());
    const std::string root_associated = "lake pki authority/" + input.customer_id + "/root";
    const std::string intermediate_associated =
        "lake pki authority/" + input.customer_id + "/intermediate";
    auto root = secrets_crypto::seal(kek, input.root_private_key_pem, root_associated);
    auto intermediate =
        secrets_crypto::seal(kek, input.intermediate_private_key_pem, intermediate_associated);
    secrets_crypto::cleanse(kek);
    if (!root || !intermediate) {
        return std::unexpected(!root ? root.error() : intermediate.error());
    }
    const std::string now = now_iso8601();
    PkiAuthorityRecord record{new_uuid_v4(),
                              input.customer_id,
                              input.root_certificate_pem,
                              input.intermediate_chain_pem,
                              *root,
                              *intermediate,
                              now,
                              now};
    pki_authorities_.push_back(record);
    return record;
}

Result<PkiAuthorityMaterial> InMemoryStore::get_pki_authority(const std::string& customer_id,
                                                              const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto record =
        std::ranges::find_if(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == customer_id;
        });
    if (record == pki_authorities_.end()) {
        return std::unexpected(StoreError{"not_found", "customer authority does not exist"});
    }
    auto raw_kek = customer_kek_locked(customer_id, false);
    if (!raw_kek || raw_kek->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(raw_kek ? StoreError{"store_error", "customer key is invalid"}
                                       : raw_kek.error());
    }
    secrets_crypto::Key kek{};
    std::copy(raw_kek->begin(), raw_kek->end(), kek.begin());
    const std::string root_associated = "lake pki authority/" + customer_id + "/root";
    const std::string intermediate_associated =
        "lake pki authority/" + customer_id + "/intermediate";
    auto root = secrets_crypto::open(kek, record->root_key_ciphertext, root_associated);
    auto intermediate =
        secrets_crypto::open(kek, record->intermediate_key_ciphertext, intermediate_associated);
    secrets_crypto::cleanse(kek);
    if (!root || !intermediate) {
        return std::unexpected(!root ? root.error() : intermediate.error());
    }
    return PkiAuthorityMaterial{record->customer_id,
                                record->root_certificate_pem,
                                record->intermediate_chain_pem,
                                std::move(*root),
                                std::move(*intermediate)};
}

Result<PkiCallerRecord> InMemoryStore::create_pki_caller(const NewPkiCallerRecord& input) {
    std::lock_guard lock(mutex_);
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    static const std::set<std::string> attestation_types{
        "kubernetes", "oidc", "jwt", "aws", "azure"};
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "customer does not exist"});
    }
    if (input.workload_taap_id.empty() || !attestation_types.contains(input.attestation_type) ||
        input.subject.empty() || !input.attributes.is_object() ||
        input.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "caller record is invalid"});
    }
    PkiCallerRecord record{new_uuid_v4(),
                           input.customer_id,
                           input.workload_taap_id,
                           input.attestation_type,
                           input.issuer,
                           input.subject,
                           input.attributes,
                           input.expires_at,
                           now_iso8601()};
    pki_callers_.push_back(record);
    return record;
}

Result<std::vector<PkiCallerRecord>> InMemoryStore::list_pki_callers(const std::string& customer_id,
                                                                     int limit) {
    if (limit < 1 || limit > 100) {
        return std::unexpected(StoreError{"invalid_input", "limit must be between 1 and 100"});
    }
    std::lock_guard lock(mutex_);
    std::vector<PkiCallerRecord> output;
    for (auto record = pki_callers_.rbegin();
         record != pki_callers_.rend() && output.size() < limit;
         ++record) {
        if (record->customer_id == customer_id) {
            output.push_back(*record);
        }
    }
    return output;
}

Result<PkiCertificateRecord>
InMemoryStore::create_pki_certificate(const NewPkiCertificateRecord& input) {
    std::lock_guard lock(mutex_);
    const auto authority = std::ranges::find_if(pki_authorities_, [&](const auto& value) {
        return value.customer_id == input.customer_id;
    });
    if (authority == pki_authorities_.end()) {
        return std::unexpected(StoreError{"not_found", "customer authority does not exist"});
    }
    if (std::ranges::any_of(pki_certificates_, [&](const auto& value) {
            return value.authority_id == authority->id && value.serial == input.serial;
        })) {
        return std::unexpected(StoreError{"conflict", "certificate serial already exists"});
    }
    pki_certificates_.push_back(PkiCertificateRecord{new_uuid_v4(),
                                                     authority->id,
                                                     input.customer_id,
                                                     input.profile,
                                                     input.subject,
                                                     input.serial,
                                                     input.public_value,
                                                     input.expires_at,
                                                     now_iso8601(),
                                                     input.renewed_from_id,
                                                     ""});
    return pki_certificates_.back();
}

Result<PkiCertificateRecord> InMemoryStore::get_pki_certificate(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(pki_certificates_, id, &PkiCertificateRecord::id);
    if (found == pki_certificates_.end()) {
        return std::unexpected(StoreError{"not_found", "certificate does not exist"});
    }
    return *found;
}

Result<PkiCertificateRecord> InMemoryStore::revoke_pki_certificate(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(pki_certificates_, id, &PkiCertificateRecord::id);
    if (found == pki_certificates_.end()) {
        return std::unexpected(StoreError{"not_found", "certificate does not exist"});
    }
    if (found->revoked_at.empty()) {
        found->revoked_at = now_iso8601();
    }
    return *found;
}

Result<AgentSession> InMemoryStore::create_agent_session(const NewAgentSession& input) {
    std::lock_guard lock(mutex_);
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "customer does not exist"});
    }
    if (input.id.empty() || input.durable_agent_taap_id.empty() ||
        input.dispatcher_taap_id.empty() || input.session_taap_id.empty() ||
        input.public_key.empty() || input.scopes.empty() || input.attestation.empty() ||
        input.transparency_root.empty() || input.issued_at.empty() ||
        (input.expires_at == "never"
             ? input.expires_unix != 0
             : (input.expires_unix <= 0 || input.expires_at <= input.issued_at))) {
        return std::unexpected(StoreError{"invalid_input", "agent session record is invalid"});
    }
    if (std::ranges::any_of(agent_sessions_, [&](const AgentSession& value) {
            return value.id == input.id || value.session_taap_id == input.session_taap_id;
        })) {
        return std::unexpected(StoreError{"conflict", "agent session already exists"});
    }
    AgentSession record{input.id,
                        input.customer_id,
                        input.durable_agent_taap_id,
                        input.dispatcher_taap_id,
                        input.session_taap_id,
                        input.public_key,
                        input.scopes,
                        input.attestation,
                        input.transparency_index,
                        input.transparency_root,
                        input.expires_unix,
                        input.issued_at,
                        input.expires_at};
    agent_sessions_.push_back(record);
    return record;
}

Result<AgentSession> InMemoryStore::revoke_agent_session(const std::string& customer_id,
                                                         const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto session = std::ranges::find_if(agent_sessions_, [&](const AgentSession& value) {
        return value.id == id && value.customer_id == customer_id;
    });
    if (session == agent_sessions_.end()) {
        return std::unexpected(StoreError{"not_found", "agent session does not exist"});
    }
    if (session->revoked_at.empty()) {
        session->revoked_at =
            std::format("{:%FT%TZ}",
                        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    }
    return *session;
}

Result<bool>
InMemoryStore::customer_allows_non_expiring_credentials(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_,
                             [&](const Customer& value) { return value.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "customer does not exist"});
    }
    return non_expiring_customers_.contains(customer_id);
}

Result<void> InMemoryStore::set_customer_non_expiring_credentials(const std::string& customer_id,
                                                                  bool allowed) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(customers_,
                             [&](const Customer& value) { return value.id == customer_id; })) {
        return std::unexpected(StoreError{"not_found", "customer does not exist"});
    }
    if (allowed) {
        non_expiring_customers_.insert(customer_id);
    } else {
        non_expiring_customers_.erase(customer_id);
    }
    return {};
}

Result<AgentSession> InMemoryStore::get_agent_session(const std::string& customer_id,
                                                      const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto session = std::ranges::find_if(agent_sessions_, [&](const AgentSession& value) {
        return value.id == id && value.customer_id == customer_id;
    });
    if (session == agent_sessions_.end()) {
        return std::unexpected(StoreError{"not_found", "agent session does not exist"});
    }
    return *session;
}

Result<PkiRecoveryCeremonyRecord>
InMemoryStore::create_pki_recovery_ceremony(const NewPkiRecoveryCeremonyRecord& input) {
    std::lock_guard lock(mutex_);
    const auto authority =
        std::ranges::find_if(pki_authorities_, [&](const PkiAuthorityRecord& value) {
            return value.customer_id == input.customer_id;
        });
    if (authority == pki_authorities_.end()) {
        return std::unexpected(StoreError{"not_found", "customer authority does not exist"});
    }
    if (input.id.empty() || input.threshold < 2 || input.share_count < input.threshold ||
        input.share_count > 255 || input.expires_at <= now_iso8601()) {
        return std::unexpected(StoreError{"invalid_input", "recovery ceremony is invalid"});
    }
    if (std::ranges::any_of(pki_recovery_ceremonies_,
                            [&](const auto& value) { return value.id == input.id; })) {
        return std::unexpected(StoreError{"conflict", "recovery ceremony already exists"});
    }
    PkiRecoveryCeremonyRecord record{input.id,
                                     input.customer_id,
                                     authority->id,
                                     input.threshold,
                                     input.share_count,
                                     std::nullopt,
                                     input.expires_at,
                                     now_iso8601()};
    pki_recovery_ceremonies_.push_back(record);
    return record;
}

Result<PkiRecoveryCeremonyRecord>
InMemoryStore::get_pki_recovery_ceremony(const std::string& customer_id,
                                         const std::string& ceremony_id) {
    std::lock_guard lock(mutex_);
    const auto ceremony = std::ranges::find_if(pki_recovery_ceremonies_, [&](const auto& value) {
        return value.id == ceremony_id && value.customer_id == customer_id;
    });
    if (ceremony == pki_recovery_ceremonies_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
    }
    return *ceremony;
}

Result<PkiRecoveryCeremonyRecord> InMemoryStore::complete_pki_recovery_ceremony(
    const std::string& customer_id,
    const std::string& ceremony_id,
    const std::vector<PkiRecoveryShareEvidence>& evidence) {
    std::lock_guard lock(mutex_);
    auto ceremony = std::ranges::find_if(pki_recovery_ceremonies_, [&](const auto& value) {
        return value.id == ceremony_id && value.customer_id == customer_id;
    });
    if (ceremony == pki_recovery_ceremonies_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery ceremony does not exist"});
    }
    const std::string now = now_iso8601();
    if (ceremony->recovered_at || ceremony->expires_at <= now) {
        return std::unexpected(StoreError{"conflict", "recovery ceremony is not active"});
    }
    std::set<int> indices;
    for (const PkiRecoveryShareEvidence& share : evidence) {
        if (share.index < 1 || share.digest.empty() || !indices.insert(share.index).second) {
            return std::unexpected(StoreError{"invalid_input", "recovery evidence is invalid"});
        }
    }
    if (static_cast<int>(evidence.size()) < ceremony->threshold) {
        return std::unexpected(StoreError{"invalid_input", "recovery quorum is not present"});
    }
    ceremony->recovered_at = now;
    pki_recovery_evidence_[ceremony_id] = evidence;
    return *ceremony;
}

Result<SecretMountRoute> InMemoryStore::create_secret_mount_route(const SecretMountRoute& input) {
    std::lock_guard lock(mutex_);
    const auto customer_object =
        std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
            return value.object_type == "customer" && value.external_id == input.customer_id;
        });
    if (customer_object == policy_objects_.end()) {
        return std::unexpected(StoreError{"invalid_input", "customer_id is unknown"});
    }
    // The owner: the customer itself (an account's mount, no organization), or an organization
    // inside the customer's namespace.
    const bool account = input.organization_id.empty();
    const auto organization_object =
        account ? policy_objects_.end()
                : std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
                      return value.object_type == "organization" &&
                             value.external_id == input.organization_id;
                  });
    if (!account && (organization_object == policy_objects_.end() ||
                     organization_object->parent_id != customer_object->id)) {
        return std::unexpected(
            StoreError{"invalid_input", "organization_id is not inside the customer namespace"});
    }
    const std::string owner_object_id = account ? customer_object->id : organization_object->id;
    const auto workspace = std::ranges::find(workspaces_, input.workspace_id, &Workspace::id);
    if (workspace == workspaces_.end() || workspace->customer_id != input.customer_id ||
        workspace->desired == "deleted") {
        return std::unexpected(
            StoreError{"invalid_input", "workspace_id must name a live workspace of this account"});
    }
    if (input.mount_id.empty() ||
        std::ranges::any_of(secret_mount_routes_, [&](const SecretMountRoute& route) {
            return route.mount_id == input.mount_id;
        })) {
        return std::unexpected(StoreError{"conflict", "secret mount already exists"});
    }
    const std::string now = now_iso8601();
    const std::string environment_external = secret_mount_environment(input);
    auto environment = std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
        return value.object_type == "environment" && value.external_id == environment_external;
    });
    if (environment == policy_objects_.end()) {
        policy_objects_.push_back(PolicyObject{
            new_uuid_v4(), "environment", environment_external, owner_object_id, now, now, true});
        environment = std::prev(policy_objects_.end());
    }
    policy_objects_.push_back(PolicyObject{
        new_uuid_v4(), "secret_mount", input.mount_id, environment->id, now, now, true});
    SecretMountRoute route = input;
    route.created_at = now;
    secret_mount_routes_.push_back(route);
    return route;
}

Result<SecretMountRoute> InMemoryStore::get_secret_mount_route(const std::string& mount_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(secret_mount_routes_, mount_id, &SecretMountRoute::mount_id);
    if (found == secret_mount_routes_.end()) {
        return std::unexpected(StoreError{"not_found", "no secret mount " + mount_id});
    }
    return *found;
}

Result<std::vector<SecretMountRoute>>
InMemoryStore::list_secret_mount_routes(const std::string& customer_id,
                                        const std::string& workspace_id) {
    std::lock_guard lock(mutex_);
    std::vector<SecretMountRoute> routes;
    for (auto route = secret_mount_routes_.rbegin(); route != secret_mount_routes_.rend();
         ++route) {
        if (route->customer_id == customer_id &&
            (workspace_id.empty() || route->workspace_id == workspace_id)) {
            routes.push_back(*route);
        }
    }
    return routes;
}

Result<WrappedDataKey> InMemoryStore::wrap_secret_data_key(const std::string& customer_id,
                                                           const std::string& owner_id,
                                                           std::uint64_t key_version,
                                                           const std::string& raw_key,
                                                           const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    return wrap_data_key_locked(customer_id, owner_id, key_version, raw_key);
}

Result<WrappedDataKey> InMemoryStore::wrap_data_key_locked(const std::string& customer_id,
                                                           const std::string& owner_id,
                                                           std::uint64_t key_version,
                                                           const std::string& raw_key) {
    auto customer_key = customer_kek_locked(customer_id, true);
    if (!customer_key || customer_key->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(customer_key ? StoreError{"store_error", "customer key is invalid"}
                                            : customer_key.error());
    }
    const auto record = std::ranges::find(customer_keks_, customer_id,
                                          &CustomerKekRecord::customer_id);
    secrets_crypto::Key kek{};
    std::copy(customer_key->begin(), customer_key->end(), kek.begin());
    const std::string associated =
        "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(key_version);
    auto wrapped = secrets_crypto::seal(kek, raw_key, associated);
    secrets_crypto::cleanse(kek);
    if (!wrapped) {
        return std::unexpected(wrapped.error());
    }
    return WrappedDataKey{record->version, *wrapped};
}

Result<std::string> InMemoryStore::unwrap_secret_data_key(const std::string& customer_id,
                                                          const std::string& owner_id,
                                                          std::uint64_t key_version,
                                                          const WrappedDataKey& wrapped,
                                                          const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find(customer_keks_, customer_id,
                                          &CustomerKekRecord::customer_id);
    if (record == customer_keks_.end()) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    const std::string* raw_kek = wrapped.kek_version == record->version ? &record->key
                                 : record->previous_version != 0 &&
                                         wrapped.kek_version == record->previous_version
                                     ? &record->previous_key
                                     : nullptr;
    if (raw_kek == nullptr || raw_kek->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(StoreError{
            "not_found", "no customer key version " + std::to_string(wrapped.kek_version) +
                             " exists"});
    }
    secrets_crypto::Key kek{};
    std::copy(raw_kek->begin(), raw_kek->end(), kek.begin());
    const std::string associated =
        "lake data key/" + customer_id + "/" + owner_id + "/" + std::to_string(key_version);
    auto opened = secrets_crypto::open(kek, wrapped.wrapped_key, associated);
    secrets_crypto::cleanse(kek);
    if (!opened || opened->size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(opened ? StoreError{"store_error", "data key is invalid"}
                                      : opened.error());
    }
    return opened;
}

Result<CustomerKekState> InMemoryStore::customer_kek_state(const std::string& customer_id) {
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find(customer_keks_, customer_id,
                                          &CustomerKekRecord::customer_id);
    if (record == customer_keks_.end()) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    return CustomerKekState{record->version, record->previous_version};
}

Result<void> InMemoryStore::retire_customer_kek(const std::string& customer_id,
                                                std::uint64_t previous_version) {
    std::lock_guard lock(mutex_);
    const auto record = std::ranges::find(customer_keks_, customer_id,
                                          &CustomerKekRecord::customer_id);
    if (record != customer_keks_.end() && record->previous_version == previous_version) {
        std::ranges::fill(record->previous_key, '\0');
        record->previous_key.clear();
        record->previous_version = 0;
    }
    return {};
}

namespace {

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Result<SecretLeaseRoute> InMemoryStore::record_secret_lease_route(const SecretLeaseRoute& input) {
    std::lock_guard lock(mutex_);
    if (const auto found =
            std::ranges::find(secret_lease_routes_, input.lease_id, &SecretLeaseRoute::lease_id);
        found != secret_lease_routes_.end()) {
        return *found;
    }
    const auto mount =
        std::ranges::find(secret_mount_routes_, input.mount_id, &SecretMountRoute::mount_id);
    if (input.lease_id.empty() || mount == secret_mount_routes_.end() ||
        mount->workspace_id != input.workspace_id || mount->customer_id != input.customer_id) {
        return std::unexpected(StoreError{
            "invalid_input", "a lease's route must name its mount's workspace"});
    }
    SecretLeaseRoute route = input;
    route.created_us = now_us();
    secret_lease_routes_.push_back(route);
    return route;
}

Result<SecretLeaseRoute> InMemoryStore::get_secret_lease_route(const std::string& lease_id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(secret_lease_routes_, lease_id, &SecretLeaseRoute::lease_id);
    if (found == secret_lease_routes_.end()) {
        return std::unexpected(StoreError{"not_found", "dynamic-secret lease does not exist"});
    }
    return *found;
}

Result<void> InMemoryStore::reschedule_secret_lease(const std::string& lease_id,
                                                    std::int64_t due_us) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(secret_lease_routes_, lease_id, &SecretLeaseRoute::lease_id);
    if (found != secret_lease_routes_.end() && found->due_us != 0) {
        found->due_us = std::max<std::int64_t>(due_us, 0);
    }
    return {};
}

Result<void> InMemoryStore::delete_secret_lease_route(const std::string& lease_id) {
    std::lock_guard lock(mutex_);
    std::erase_if(secret_lease_routes_,
                  [&](const SecretLeaseRoute& route) { return route.lease_id == lease_id; });
    return {};
}

Result<std::vector<SecretLeaseRoute>> InMemoryStore::claim_due_secret_leases(int limit,
                                                                             int hold_seconds) {
    std::lock_guard lock(mutex_);
    const std::int64_t now = now_us();
    std::vector<SecretLeaseRoute*> due;
    for (SecretLeaseRoute& route : secret_lease_routes_) {
        if (route.due_us != 0 && route.due_us <= now) {
            due.push_back(&route);
        }
    }
    std::ranges::sort(due, {}, [](const SecretLeaseRoute* route) { return route->due_us; });
    std::vector<SecretLeaseRoute> claimed;
    for (SecretLeaseRoute* route : due) {
        if (static_cast<int>(claimed.size()) >= std::clamp(limit, 1, 5000)) {
            break;
        }
        claimed.push_back(*route);
        route->due_us = now + std::int64_t{std::max(hold_seconds, 1)} * 1'000'000;
    }
    return claimed;
}

Result<CustomerKekRotation> InMemoryStore::rotate_customer_kek(const std::string& customer_id,
                                                               const std::string& instance_kek) {
    static_cast<void>(instance_kek);
    std::lock_guard lock(mutex_);
    auto current = std::ranges::find_if(customer_keks_, [&](const CustomerKekRecord& value) {
        return value.customer_id == customer_id;
    });
    if (current == customer_keks_.end() || current->key.size() != secrets_crypto::Key{}.size()) {
        return std::unexpected(StoreError{"not_found", "no customer key exists"});
    }
    if (current->previous_version != 0) {
        return std::unexpected(StoreError{
            "conflict", "an earlier customer KEK rotation is still rewrapping data keys"});
    }
    auto generated = secrets_crypto::random_key();
    if (!generated) {
        return std::unexpected(generated.error());
    }
    secrets_crypto::Key old_key{};
    std::copy(current->key.begin(), current->key.end(), old_key.begin());
    const std::uint64_t version = current->version + 1;
    std::vector<std::pair<std::string, std::string>> authority_replacements;
    authority_replacements.reserve(pki_authorities_.size());
    for (const PkiAuthorityRecord& record : pki_authorities_) {
        if (record.customer_id != customer_id) {
            authority_replacements.emplace_back();
            continue;
        }
        const std::string root_associated = "lake pki authority/" + customer_id + "/root";
        const std::string intermediate_associated =
            "lake pki authority/" + customer_id + "/intermediate";
        auto root = secrets_crypto::open(old_key, record.root_key_ciphertext, root_associated);
        auto intermediate = secrets_crypto::open(
            old_key, record.intermediate_key_ciphertext, intermediate_associated);
        if (!root || !intermediate) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(!root ? root.error() : intermediate.error());
        }
        auto wrapped_root = secrets_crypto::seal(*generated, *root, root_associated);
        auto wrapped_intermediate =
            secrets_crypto::seal(*generated, *intermediate, intermediate_associated);
        if (!wrapped_root || !wrapped_intermediate) {
            secrets_crypto::cleanse(old_key);
            secrets_crypto::cleanse(*generated);
            return std::unexpected(!wrapped_root ? wrapped_root.error()
                                                 : wrapped_intermediate.error());
        }
        authority_replacements.emplace_back(*wrapped_root, *wrapped_intermediate);
    }
    secrets_crypto::cleanse(old_key);
    std::uint64_t rewrapped = 0;
    for (std::size_t index = 0; index < pki_authorities_.size(); ++index) {
        if (!authority_replacements[index].first.empty()) {
            pki_authorities_[index].root_key_ciphertext =
                std::move(authority_replacements[index].first);
            pki_authorities_[index].intermediate_key_ciphertext =
                std::move(authority_replacements[index].second);
            ++rewrapped;
        }
    }
    const std::uint64_t previous = current->version;
    current->previous_key = std::move(current->key);
    current->previous_version = previous;
    current->key.assign(reinterpret_cast<const char*>(generated->data()), generated->size());
    secrets_crypto::cleanse(*generated);
    current->version = version;
    return CustomerKekRotation{customer_id, version, rewrapped, previous};
}

Result<OfflineGrant> InMemoryStore::issue_offline_grant(const NewOfflineGrant& input) {
    if (input.id.empty() || input.user_id.empty() || input.principal_taap_id.empty() ||
        input.device_id.empty() || input.scopes.empty() || input.duration_seconds <= 0 ||
        input.duration_seconds > 604800 || !input.credential.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "offline grant is incomplete"});
    }
    std::lock_guard lock(mutex_);
    const bool device_exists = std::ranges::any_of(devices_, [&](const Device& device) {
        return device.id == input.device_id && device.user_id == input.user_id &&
               device.revoked_at.empty();
    });
    if (!device_exists) {
        return std::unexpected(StoreError{"not_found", "active device not found"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto expires = now + std::chrono::seconds(input.duration_seconds);
    OfflineGrant grant{input.id,
                       input.user_id,
                       input.principal_taap_id,
                       input.device_id,
                       input.scopes,
                       now_iso8601(),
                       iso8601_at(expires),
                       {},
                       input.credential};
    offline_grants_.push_back(grant);
    return grant;
}

Result<std::vector<OfflineGrant>> InMemoryStore::list_offline_grants(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<OfflineGrant> output;
    for (const OfflineGrant& grant : offline_grants_) {
        if (grant.user_id == user_id) {
            output.push_back(grant);
        }
    }
    return output;
}

Result<OfflineGrant> InMemoryStore::get_offline_grant(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(offline_grants_, id, &OfflineGrant::id);
    if (found == offline_grants_.end()) {
        return std::unexpected(StoreError{"not_found", "offline grant not found"});
    }
    return *found;
}

Result<OfflineGrant> InMemoryStore::revoke_offline_grant(const std::string& user_id,
                                                         const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(offline_grants_, id, &OfflineGrant::id);
    if (found == offline_grants_.end() || found->user_id != user_id) {
        return std::unexpected(StoreError{"not_found", "offline grant not found"});
    }
    if (found->revoked_at.empty()) {
        found->revoked_at = now_iso8601();
    }
    return *found;
}

Result<IssuedSession> InMemoryStore::recover_with_code(const std::string& pool_id,
                                                       const std::string& email,
                                                       const std::string& code,
                                                       const std::string& device_name) {
    std::lock_guard lock(mutex_);
    for (PasswordCredential& credential : passwords_) {
        if (credential.pool_id != pool_id || lower(credential.email) != lower(email)) {
            continue;
        }
        const auto recovery =
            std::ranges::find_if(credential.recovery_hashes, [&](const std::string& hash) {
                return identity_crypto::verify_secret(hash, code);
            });
        if (recovery == credential.recovery_hashes.end()) {
            break;
        }
        credential.recovery_hashes.erase(recovery);
        Device& device = add_device(devices_, credential.user_id, device_name);
        IssuedSession issued = add_session(
            sessions_, credential.user_id, pool_id, device.id, "browser", "", {"*"}, "", 86400);
        if (issued.secret.empty()) {
            return std::unexpected(StoreError{"store_error", "could not issue recovery session"});
        }
        issued.session.reason.clear();
        return issued;
    }
    return std::unexpected(StoreError{"unauthorized", "invalid email or recovery code"});
}

namespace {

void add_recovery_notifications(std::vector<RecoveryNotification>& notifications,
                                const std::vector<Device>& devices,
                                const RecoveryAttempt& attempt,
                                const std::string& kind) {
    const std::string now = now_iso8601();
    for (const Device& device : devices) {
        if (device.user_id == attempt.user_id) {
            notifications.push_back(
                {new_uuid_v4(), attempt.id, attempt.user_id, device.id, kind, now});
        }
    }
}

} // namespace

Result<RecoveryAttempt> InMemoryStore::begin_email_recovery(const std::string& pool_id,
                                                            const std::string& email,
                                                            int cooling_seconds) {
    if (cooling_seconds < 3600) {
        return std::unexpected(
            StoreError{"invalid_input", "email recovery cooling period is too short"});
    }
    std::lock_guard lock(mutex_);
    const auto password = std::ranges::find_if(passwords_, [&](const PasswordCredential& value) {
        return value.pool_id == pool_id && lower(value.email) == lower(email);
    });
    if (password == passwords_.end()) {
        return std::unexpected(StoreError{"not_found", "account not found"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto ready = now + std::chrono::seconds(cooling_seconds);
    const auto expires = ready + std::chrono::hours(24);
    const std::string token = identity_crypto::random_secret();
    const std::string token_hash = identity_crypto::hash_secret(token);
    if (token.empty() || token_hash.empty()) {
        return std::unexpected(StoreError{"store_error", "could not create recovery token"});
    }
    RecoveryAttempt attempt{new_uuid_v4(),
                            password->user_id,
                            {},
                            "email",
                            "cooling",
                            iso8601_at(ready),
                            iso8601_at(expires),
                            {},
                            0};
    recovery_attempts_.push_back({attempt, token_hash, {}, ready, expires});
    recovery_deliveries_.push_back(
        {new_uuid_v4(), attempt.id, email, token, "email_recovery_started", now_iso8601(), {}});
    add_recovery_notifications(
        recovery_notifications_, devices_, attempt, "email_recovery_started");
    return attempt;
}

Result<RecoveryAttempt> InMemoryStore::begin_admin_recovery(const std::string& organization_id,
                                                            const std::string& user_id,
                                                            const std::string& requested_by) {
    std::lock_guard lock(mutex_);
    const auto requester = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == organization_id &&
               membership.principal_taap_id == requested_by &&
               (membership.role == "owner" || membership.role == "admin");
    });
    const auto target = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == organization_id &&
               membership.principal_type == "user" && membership.principal_id == user_id;
    });
    if (requester == memberships_.end()) {
        return std::unexpected(
            StoreError{"unauthorized", "requester is not an organization admin"});
    }
    if (target == memberships_.end()) {
        return std::unexpected(StoreError{"not_found", "target user is not in the organization"});
    }
    const auto now = std::chrono::system_clock::now();
    const auto expires = now + std::chrono::hours(24);
    RecoveryAttempt attempt{new_uuid_v4(),
                            user_id,
                            organization_id,
                            "admin",
                            "pending_approval",
                            iso8601_at(now),
                            iso8601_at(expires),
                            {},
                            1};
    recovery_attempts_.push_back({attempt, {}, {requested_by}, now, expires});
    add_recovery_notifications(
        recovery_notifications_, devices_, attempt, "admin_recovery_requested");
    return attempt;
}

Result<RecoveryAttempt> InMemoryStore::get_recovery_attempt(const std::string& recovery_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end()) {
        return std::unexpected(StoreError{"not_found", "recovery attempt not found"});
    }
    return found->attempt;
}

Result<RecoveryAttempt> InMemoryStore::approve_admin_recovery(const std::string& recovery_id,
                                                              const std::string& approved_by) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end() || found->attempt.method != "admin") {
        return std::unexpected(StoreError{"not_found", "admin recovery not found"});
    }
    const auto admin = std::ranges::find_if(memberships_, [&](const Membership& membership) {
        return membership.organization_id == found->attempt.organization_id &&
               membership.principal_taap_id == approved_by &&
               (membership.role == "owner" || membership.role == "admin");
    });
    if (admin == memberships_.end()) {
        return std::unexpected(StoreError{"unauthorized", "approver is not an organization admin"});
    }
    if (std::ranges::find(found->approvals, approved_by) != found->approvals.end()) {
        return std::unexpected(
            StoreError{"conflict", "a different organization admin must approve recovery"});
    }
    if (std::chrono::system_clock::now() >= found->expires_at) {
        return std::unexpected(StoreError{"conflict", "admin recovery has expired"});
    }
    found->approvals.push_back(approved_by);
    found->attempt.approval_count = static_cast<int>(found->approvals.size());
    if (found->approvals.size() == 2) {
        const std::string token = identity_crypto::random_secret();
        found->token_hash = identity_crypto::hash_secret(token);
        if (token.empty() || found->token_hash.empty()) {
            return std::unexpected(StoreError{"store_error", "could not create recovery token"});
        }
        found->attempt.status = "approved";
        const auto user = std::ranges::find(users_, found->attempt.user_id, &User::id);
        if (user == users_.end()) {
            return std::unexpected(StoreError{"not_found", "target user not found"});
        }
        recovery_deliveries_.push_back({new_uuid_v4(),
                                        found->attempt.id,
                                        user->email,
                                        token,
                                        "admin_recovery_approved",
                                        now_iso8601(),
                                        {}});
        add_recovery_notifications(
            recovery_notifications_, devices_, found->attempt, "admin_recovery_approved");
    }
    return found->attempt;
}

Result<IssuedSession> InMemoryStore::complete_recovery(const std::string& recovery_id,
                                                       const std::string& token,
                                                       const std::string& device_name) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(recovery_attempts_, [&](const RecoveryRecord& value) {
        return value.attempt.id == recovery_id;
    });
    if (found == recovery_attempts_.end() || found->attempt.status == "completed" ||
        found->token_hash.empty() || !identity_crypto::verify_secret(found->token_hash, token)) {
        return std::unexpected(StoreError{"unauthorized", "invalid recovery token"});
    }
    const auto now = std::chrono::system_clock::now();
    if (now < found->ready_at) {
        return std::unexpected(StoreError{"conflict", "recovery cooling period has not ended"});
    }
    if (now >= found->expires_at) {
        return std::unexpected(StoreError{"conflict", "recovery token has expired"});
    }
    if (found->attempt.method == "admin" && found->approvals.size() != 2) {
        return std::unexpected(StoreError{"conflict", "two administrator approvals are required"});
    }
    const std::string revoked_at = now_iso8601();
    for (Session& session : sessions_) {
        if (session.user_id == found->attempt.user_id && session.revoked_at.empty()) {
            session.revoked_at = revoked_at;
        }
    }
    const auto password = std::ranges::find_if(passwords_, [&](const PasswordCredential& value) {
        return value.user_id == found->attempt.user_id;
    });
    if (password == passwords_.end()) {
        return std::unexpected(StoreError{"not_found", "password credential not found"});
    }
    found->attempt.status = "completed";
    found->attempt.completed_at = revoked_at;
    add_recovery_notifications(
        recovery_notifications_, devices_, found->attempt, "account_recovery_completed");
    Device& device = add_device(devices_, found->attempt.user_id, device_name);
    IssuedSession issued = add_session(sessions_,
                                       found->attempt.user_id,
                                       password->pool_id,
                                       device.id,
                                       "browser",
                                       "",
                                       {"*"},
                                       "",
                                       86400);
    if (issued.secret.empty()) {
        return std::unexpected(StoreError{"store_error", "could not issue recovery session"});
    }
    issued.session.reason.clear();
    return issued;
}

Result<std::vector<RecoveryDelivery>> InMemoryStore::claim_recovery_deliveries(int limit) {
    std::lock_guard lock(mutex_);
    std::vector<RecoveryDelivery> output;
    for (RecoveryDelivery& delivery : recovery_deliveries_) {
        if (!delivery.delivered_at.empty()) {
            continue;
        }
        delivery.delivered_at = now_iso8601();
        output.push_back(delivery);
        delivery.token.clear();
        if (output.size() == static_cast<std::size_t>(limit)) {
            break;
        }
    }
    return output;
}

Result<std::vector<RecoveryNotification>>
InMemoryStore::list_recovery_notifications(const std::string& user_id) {
    std::lock_guard lock(mutex_);
    std::vector<RecoveryNotification> output;
    for (const RecoveryNotification& notification : recovery_notifications_) {
        if (notification.user_id == user_id) {
            output.push_back(notification);
        }
    }
    return output;
}

Result<RecoveryContact> InMemoryStore::configure_recovery_contact(const RecoveryContact& input) {
    if (input.repository_id.empty() || input.contact_taap_id.empty() || input.wrapped_key.empty()) {
        return std::unexpected(StoreError{"invalid_input", "recovery contact is incomplete"});
    }
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(repositories_, [&](const Repository& value) {
            return value.id == input.repository_id;
        })) {
        return std::unexpected(StoreError{"not_found", "repository not found"});
    }
    const auto found =
        std::ranges::find(recovery_contacts_, input.repository_id, &RecoveryContact::repository_id);
    const std::string now = now_iso8601();
    if (found != recovery_contacts_.end()) {
        found->contact_taap_id = input.contact_taap_id;
        found->wrapped_key = input.wrapped_key;
        found->updated_at = now;
        return *found;
    }
    recovery_contacts_.push_back(
        {input.repository_id, input.contact_taap_id, input.wrapped_key, now, now});
    return recovery_contacts_.back();
}

Result<RecoveryContact> InMemoryStore::get_recovery_contact(const std::string& repository_id) {
    std::lock_guard lock(mutex_);
    const auto found =
        std::ranges::find(recovery_contacts_, repository_id, &RecoveryContact::repository_id);
    if (found == recovery_contacts_.end()) {
        return std::unexpected(StoreError{"not_found", "repository has no recovery contact"});
    }
    return *found;
}

} // namespace lake::control
