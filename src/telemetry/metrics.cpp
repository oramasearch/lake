#include "lake/telemetry/metrics.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>

#include "lake/control/store.hpp"

namespace lake::telemetry {
namespace {

constexpr std::array<std::pair<double, std::string_view>, 11> latency_buckets = {
    {{0.001, "0.001"},
     {0.005, "0.005"},
     {0.01, "0.01"},
     {0.025, "0.025"},
     {0.05, "0.05"},
     {0.1, "0.1"},
     {0.25, "0.25"},
     {0.5, "0.5"},
     {1.0, "1"},
     {2.5, "2.5"},
     {5.0, "5"}},
};

struct SeriesKey {
    std::string name;
    Labels labels;
    auto operator<=>(const SeriesKey&) const = default;
};

struct Histogram {
    std::array<std::uint64_t, latency_buckets.size()> buckets{};
    std::uint64_t count = 0;
    double sum = 0.0;
};

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

std::string escaped_label(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == '"')
            out += '\\';
        if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string labels_text(const Labels& labels,
                        std::string_view extra_name = {},
                        std::string_view extra_value = {}) {
    if (labels.empty() && extra_name.empty())
        return {};
    std::string out = "{";
    bool first = true;
    auto append = [&](std::string_view name, std::string_view value) {
        if (!first)
            out += ',';
        first = false;
        out += name;
        out += "=\"";
        out += escaped_label(value);
        out += '"';
    };
    for (const auto& [name, value] : labels)
        append(name, value);
    if (!extra_name.empty())
        append(extra_name, extra_value);
    out += '}';
    return out;
}

template <class Result> std::string result_label(const Result& result) {
    return result ? "ok" : result.error().code;
}

} // namespace

struct Metrics::Impl {
    mutable std::mutex mutex;
    std::map<SeriesKey, double> counters;
    std::map<SeriesKey, double> gauges;
    std::map<SeriesKey, Histogram> histograms;
};

Metrics::Metrics() : impl_(std::make_unique<Impl>()) {}
Metrics::~Metrics() = default;

void Metrics::increment_counter(std::string name, Labels labels, double amount) {
    std::lock_guard lock(impl_->mutex);
    impl_->counters[SeriesKey{std::move(name), std::move(labels)}] += amount;
}

void Metrics::set_gauge(std::string name, Labels labels, double value) {
    std::lock_guard lock(impl_->mutex);
    impl_->gauges[SeriesKey{std::move(name), std::move(labels)}] = value;
}

void Metrics::observe_histogram(std::string name, Labels labels, double value) {
    std::lock_guard lock(impl_->mutex);
    Histogram& histogram = impl_->histograms[SeriesKey{std::move(name), std::move(labels)}];
    for (std::size_t i = 0; i < latency_buckets.size(); ++i) {
        if (value <= latency_buckets[i].first)
            ++histogram.buckets[i];
    }
    ++histogram.count;
    histogram.sum += value;
}

std::string Metrics::prometheus_text() const {
    std::lock_guard lock(impl_->mutex);
    std::string out;

    auto scalar_families = [&](const auto& samples, std::string_view type) {
        std::string current_name;
        for (const auto& [key, value] : samples) {
            if (key.name != current_name) {
                current_name = key.name;
                out += "# TYPE " + current_name + " " + std::string(type) + "\n";
            }
            out += key.name + labels_text(key.labels) + " " + number(value) + "\n";
        }
    };
    scalar_families(impl_->counters, "counter");
    scalar_families(impl_->gauges, "gauge");

    std::string current_name;
    for (const auto& [key, histogram] : impl_->histograms) {
        if (key.name != current_name) {
            current_name = key.name;
            out += "# TYPE " + current_name + " histogram\n";
        }
        for (std::size_t i = 0; i < latency_buckets.size(); ++i) {
            out += key.name + "_bucket" + labels_text(key.labels, "le", latency_buckets[i].second) +
                   " " + std::to_string(histogram.buckets[i]) + "\n";
        }
        out += key.name + "_bucket" + labels_text(key.labels, "le", "+Inf") + " " +
               std::to_string(histogram.count) + "\n";
        out += key.name + "_sum" + labels_text(key.labels) + " " + number(histogram.sum) + "\n";
        out += key.name + "_count" + labels_text(key.labels) + " " +
               std::to_string(histogram.count) + "\n";
    }
    return out;
}

namespace {

class InstrumentedStore final : public control::Store {
private:
    template <class Function> auto call(std::string_view operation, Function&& function) {
        const auto start = std::chrono::steady_clock::now();
        auto result = function();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const Labels labels{{"store", kind_},
                            {"operation", std::string(operation)},
                            {"result", result_label(result)}};
        metrics_.increment_counter("lake_store_calls_total", labels);
        metrics_.observe_histogram("lake_store_call_duration_seconds", labels, seconds);
        return result;
    }

public:
    InstrumentedStore(std::shared_ptr<control::Store> store, Metrics& metrics)
        : store_(std::move(store)), metrics_(metrics), kind_(store_->kind()) {}

    std::string kind() const override {
        return kind_;
    }

    control::Result<control::NetworkConnection>
    create_network_connection(const control::NetworkScope& scope,
                              const control::NetworkConnectionSpec& spec,
                              const control::NetworkActor& actor) override {
        return call("create_network_connection",
                    [&] { return store_->create_network_connection(scope, spec, actor); });
    }
    control::Result<control::NetworkConnection>
    update_network_connection(const control::NetworkScope& scope,
                              const std::string& id,
                              const control::NetworkConnectionSpec& spec,
                              const control::NetworkActor& actor) override {
        return call("update_network_connection",
                    [&] { return store_->update_network_connection(scope, id, spec, actor); });
    }
    control::Result<control::NetworkConnection>
    withdraw_network_connection(const control::NetworkScope& scope,
                                const std::string& id,
                                const control::NetworkActor& actor) override {
        return call("withdraw_network_connection",
                    [&] { return store_->withdraw_network_connection(scope, id, actor); });
    }
    control::Result<control::NetworkConnection>
    get_network_connection(const control::NetworkScope& scope, const std::string& id) override {
        return call("get_network_connection",
                    [&] { return store_->get_network_connection(scope, id); });
    }
    control::Result<std::vector<control::NetworkConnection>>
    list_network_connections(const control::NetworkScope& scope) override {
        return call("list_network_connections",
                    [&] { return store_->list_network_connections(scope); });
    }

    control::Result<control::NetworkEgressRule>
    create_network_egress_rule(const control::NetworkScope& scope,
                               const control::NetworkEgressRuleSpec& spec,
                               const control::NetworkActor& actor) override {
        return call("create_network_egress_rule",
                    [&] { return store_->create_network_egress_rule(scope, spec, actor); });
    }
    control::Result<control::NetworkEgressRule>
    update_network_egress_rule(const control::NetworkScope& scope,
                               const std::string& id,
                               const control::NetworkEgressRuleSpec& spec,
                               const control::NetworkActor& actor) override {
        return call("update_network_egress_rule",
                    [&] { return store_->update_network_egress_rule(scope, id, spec, actor); });
    }
    control::Result<control::NetworkEgressRule>
    withdraw_network_egress_rule(const control::NetworkScope& scope,
                                 const std::string& id,
                                 const control::NetworkActor& actor) override {
        return call("withdraw_network_egress_rule",
                    [&] { return store_->withdraw_network_egress_rule(scope, id, actor); });
    }
    control::Result<control::NetworkEgressRule>
    get_network_egress_rule(const control::NetworkScope& scope, const std::string& id) override {
        return call("get_network_egress_rule",
                    [&] { return store_->get_network_egress_rule(scope, id); });
    }
    control::Result<std::vector<control::NetworkEgressRule>>
    list_network_egress_rules(const control::NetworkScope& scope) override {
        return call("list_network_egress_rules",
                    [&] { return store_->list_network_egress_rules(scope); });
    }

    control::Result<control::NetworkTunnelGrant>
    create_network_tunnel_grant(const control::NetworkScope& scope,
                                const control::NetworkTunnelGrantSpec& spec,
                                const control::NetworkActor& actor,
                                const control::NetworkTunnelGrantLimits& limits) override {
        return call("create_network_tunnel_grant", [&] {
            return store_->create_network_tunnel_grant(scope, spec, actor, limits);
        });
    }
    control::Result<control::NetworkTunnelGrant>
    update_network_tunnel_grant(const control::NetworkScope& scope,
                                const std::string& id,
                                const control::NetworkTunnelGrantSpec& spec,
                                const control::NetworkActor& actor) override {
        return call("update_network_tunnel_grant",
                    [&] { return store_->update_network_tunnel_grant(scope, id, spec, actor); });
    }
    control::Result<control::NetworkTunnelGrant>
    revoke_network_tunnel_grant(const control::NetworkScope& scope,
                                const std::string& id,
                                const control::NetworkActor& actor) override {
        return call("revoke_network_tunnel_grant",
                    [&] { return store_->revoke_network_tunnel_grant(scope, id, actor); });
    }
    control::Result<control::NetworkTunnelGrant>
    get_network_tunnel_grant(const control::NetworkScope& scope, const std::string& id) override {
        return call("get_network_tunnel_grant",
                    [&] { return store_->get_network_tunnel_grant(scope, id); });
    }
    control::Result<std::vector<control::NetworkTunnelGrant>>
    list_network_tunnel_grants(const control::NetworkScope& scope) override {
        return call("list_network_tunnel_grants",
                    [&] { return store_->list_network_tunnel_grants(scope); });
    }

    control::Result<control::NetworkPeer>
    create_network_peer(const control::NetworkScope& scope,
                        const control::NetworkPeerSpec& spec,
                        const control::NetworkActor& actor) override {
        return call("create_network_peer",
                    [&] { return store_->create_network_peer(scope, spec, actor); });
    }
    control::Result<control::NetworkPeer>
    update_network_peer(const control::NetworkScope& scope,
                        const std::string& id,
                        const control::NetworkPeerSpec& spec,
                        const control::NetworkActor& actor) override {
        return call("update_network_peer",
                    [&] { return store_->update_network_peer(scope, id, spec, actor); });
    }
    control::Result<control::NetworkPeer>
    withdraw_network_peer(const control::NetworkScope& scope,
                          const std::string& id,
                          const control::NetworkActor& actor) override {
        return call("withdraw_network_peer",
                    [&] { return store_->withdraw_network_peer(scope, id, actor); });
    }
    control::Result<control::NetworkPeer> get_network_peer(const control::NetworkScope& scope,
                                                           const std::string& id) override {
        return call("get_network_peer", [&] { return store_->get_network_peer(scope, id); });
    }
    control::Result<std::vector<control::NetworkPeer>>
    list_network_peers(const control::NetworkScope& scope) override {
        return call("list_network_peers", [&] { return store_->list_network_peers(scope); });
    }

    control::Result<void> ping() override {
        return call("ping", [&] { return store_->ping(); });
    }

    control::Result<control::User> create_user(const control::NewUser& value) override {
        return call("create_user", [&] { return store_->create_user(value); });
    }
    control::Result<std::vector<control::User>> list_users(int limit) override {
        return call("list_users", [&] { return store_->list_users(limit); });
    }
    control::Result<control::User> get_user(const std::string& id) override {
        return call("get_user", [&] { return store_->get_user(id); });
    }
    control::Result<std::map<std::string, std::string>>
    principal_names(const std::vector<std::string>& taap_ids) override {
        return call("principal_names", [&] { return store_->principal_names(taap_ids); });
    }
    control::Result<control::Organization>
    create_organization(const control::NewOrganization& value) override {
        return call("create_organization", [&] { return store_->create_organization(value); });
    }
    control::Result<std::vector<control::Organization>> list_organizations(int limit) override {
        return call("list_organizations", [&] { return store_->list_organizations(limit); });
    }
    control::Result<control::Organization> get_organization(const std::string& id) override {
        return call("get_organization", [&] { return store_->get_organization(id); });
    }
    control::Result<control::OrganizationDeletion> delete_organization(const std::string& id,
                                                                       bool dry_run) override {
        return call("delete_organization",
                    [&] { return store_->delete_organization(id, dry_run); });
    }
    control::Result<control::AccountDeletion> delete_account(const std::string& user_id,
                                                             bool dry_run) override {
        return call("delete_account", [&] { return store_->delete_account(user_id, dry_run); });
    }
    control::Result<control::Customer> create_customer(const control::NewCustomer& value) override {
        return call("create_customer", [&] { return store_->create_customer(value); });
    }
    control::Result<std::vector<control::Customer>>
    list_customers(int limit, const std::string& user_id) override {
        return call("list_customers", [&] { return store_->list_customers(limit, user_id); });
    }
    control::Result<control::Customer> get_customer(const std::string& id) override {
        return call("get_customer", [&] { return store_->get_customer(id); });
    }
    control::Result<control::Customer>
    organization_customer(const std::string& organization_id) override {
        return call("organization_customer",
                    [&] { return store_->organization_customer(organization_id); });
    }
    control::Result<control::Repository>
    create_repository(const control::NewRepository& value) override {
        return call("create_repository", [&] { return store_->create_repository(value); });
    }
    control::Result<std::vector<control::Repository>>
    list_repositories(const std::string& customer_id,
                      int limit,
                      const std::string& workspace_id = {}) override {
        return call("list_repositories",
                    [&] { return store_->list_repositories(customer_id, limit, workspace_id); });
    }
    control::Result<control::Repository> get_repository(const std::string& id) override {
        return call("get_repository", [&] { return store_->get_repository(id); });
    }
    control::Result<std::vector<control::Repository>>
    repositories_by_liquid_id(const std::string& liquid_id) override {
        return call("repositories_by_liquid_id",
                    [&] { return store_->repositories_by_liquid_id(liquid_id); });
    }
    control::Result<void> set_repository_liquid_id(const std::string& id,
                                                   const std::string& liquid_id) override {
        return call("set_repository_liquid_id",
                    [&] { return store_->set_repository_liquid_id(id, liquid_id); });
    }
    control::Result<std::vector<control::Repository>>
    repositories_without_liquid_id(int limit) override {
        return call("repositories_without_liquid_id",
                    [&] { return store_->repositories_without_liquid_id(limit); });
    }
    control::Result<control::Repository> rename_repository(const std::string& id,
                                                           const std::string& name) override {
        return call("rename_repository", [&] { return store_->rename_repository(id, name); });
    }
    control::Result<control::Repository>
    set_repository_visibility(const std::string& id, const std::string& visibility) override {
        return call("set_repository_visibility",
                    [&] { return store_->set_repository_visibility(id, visibility); });
    }
    control::Result<control::Repository>
    delete_repository(const std::string& id, const control::RepositoryDeletion& deletion) override {
        return call("delete_repository", [&] { return store_->delete_repository(id, deletion); });
    }
    control::Result<void>
    assign_legacy_audit_events(const std::vector<std::string>& workspace_actions) override {
        return call("assign_legacy_audit_events",
                    [&] { return store_->assign_legacy_audit_events(workspace_actions); });
    }
    control::Result<std::vector<control::LegacyAuditEvent>>
    legacy_audit_events(const std::string& after_id, int limit) override {
        return call("legacy_audit_events",
                    [&] { return store_->legacy_audit_events(after_id, limit); });
    }
    control::Result<control::Team> create_team(const control::NewTeam& value) override {
        return call("create_team", [&] { return store_->create_team(value); });
    }
    control::Result<std::vector<control::Team>> list_teams(const std::string& organization_id,
                                                           int limit) override {
        return call("list_teams", [&] { return store_->list_teams(organization_id, limit); });
    }
    control::Result<control::Team> get_team(const std::string& id) override {
        return call("get_team", [&] { return store_->get_team(id); });
    }
    control::Result<control::Membership>
    create_membership(const control::NewMembership& value) override {
        return call("create_membership", [&] { return store_->create_membership(value); });
    }
    control::Result<control::Membership> get_membership(const std::string& id) override {
        return call("get_membership", [&] { return store_->get_membership(id); });
    }
    control::Result<std::vector<control::Membership>> list_memberships(
        const std::string& organization_id, const std::string& team_id, int limit) override {
        return call("list_memberships",
                    [&] { return store_->list_memberships(organization_id, team_id, limit); });
    }
    control::Result<control::Membership> set_member_role(const std::string& id,
                                                         const std::string& role,
                                                         const std::string& actor) override {
        return call("set_member_role", [&] { return store_->set_member_role(id, role, actor); });
    }
    control::Result<void> remove_member(const std::string& id, const std::string& actor) override {
        return call("remove_member", [&] { return store_->remove_member(id, actor); });
    }
    control::Result<control::Membership>
    accept_invitation(const control::AcceptInvitation& value) override {
        return call("accept_invitation", [&] { return store_->accept_invitation(value); });
    }
    control::Result<control::Invitation>
    create_invitation(const control::NewInvitation& value) override {
        return call("create_invitation", [&] { return store_->create_invitation(value); });
    }
    control::Result<std::vector<control::Invitation>> list_invitations(
        const std::string& organization_id, const std::string& team_id, int limit) override {
        return call("list_invitations",
                    [&] { return store_->list_invitations(organization_id, team_id, limit); });
    }
    control::Result<control::Invitation> get_invitation(const std::string& id) override {
        return call("get_invitation", [&] { return store_->get_invitation(id); });
    }
    control::Result<control::Invitation> revoke_invitation(const std::string& id,
                                                           bool dry_run) override {
        return call("revoke_invitation", [&] { return store_->revoke_invitation(id, dry_run); });
    }
    control::Result<control::OrganizationSpace>
    create_organization_space(const control::NewOrganizationSpace& value) override {
        return call("create_organization_space",
                    [&] { return store_->create_organization_space(value); });
    }
    control::Result<control::OrganizationSpace>
    convert_personal_space(const control::ConvertPersonalSpace& value) override {
        return call("convert_personal_space",
                    [&] { return store_->convert_personal_space(value); });
    }
    control::Result<std::vector<control::Membership>>
    list_user_memberships(const std::string& user_id, int limit) override {
        return call("list_user_memberships",
                    [&] { return store_->list_user_memberships(user_id, limit); });
    }
    control::Result<std::vector<control::Invitation>>
    list_invitee_invitations(const std::string& email,
                             const std::vector<std::string>& handles,
                             int limit) override {
        return call("list_invitee_invitations",
                    [&] { return store_->list_invitee_invitations(email, handles, limit); });
    }
    control::Result<control::PolicyObject>
    create_policy_object(const control::NewPolicyObject& value) override {
        return call("create_policy_object", [&] { return store_->create_policy_object(value); });
    }
    control::Result<control::PolicyObject> get_policy_object(const std::string& id) override {
        return call("get_policy_object", [&] { return store_->get_policy_object(id); });
    }
    control::Result<control::PolicyObject>
    set_policy_object_parent(const std::string& id, const std::string& parent_id) override {
        return call("set_policy_object_parent",
                    [&] { return store_->set_policy_object_parent(id, parent_id); });
    }
    control::Result<control::PolicyObject>
    find_policy_object(const std::string& object_type, const std::string& external_id) override {
        return call("find_policy_object",
                    [&] { return store_->find_policy_object(object_type, external_id); });
    }
    control::Result<std::vector<control::PolicyObject>> list_policy_objects(
        const std::string& parent_id, const std::string& object_type, int limit) override {
        return call("list_policy_objects",
                    [&] { return store_->list_policy_objects(parent_id, object_type, limit); });
    }
    control::Result<std::vector<control::ObjectPathEntry>>
    policy_paths(const std::vector<std::string>& requested_object_ids) override {
        return call("policy_paths", [&] { return store_->policy_paths(requested_object_ids); });
    }
    control::Result<control::Grant> create_grant(const control::NewGrant& value) override {
        return call("create_grant", [&] { return store_->create_grant(value); });
    }
    control::Result<control::Grant> get_grant(const std::string& id) override {
        return call("get_grant", [&] { return store_->get_grant(id); });
    }
    control::Result<control::Grant> revoke_grant(const std::string& id, bool dry_run) override {
        return call("revoke_grant", [&] { return store_->revoke_grant(id, dry_run); });
    }
    control::Result<std::vector<control::Grant>>
    list_grants(const std::vector<std::string>& object_ids,
                const std::optional<std::string>& principal_taap_id,
                int limit) override {
        return call("list_grants",
                    [&] { return store_->list_grants(object_ids, principal_taap_id, limit); });
    }
    control::Result<control::Application>
    create_application(const control::NewApplication& value) override {
        return call("create_application", [&] { return store_->create_application(value); });
    }
    control::Result<std::vector<control::Application>>
    list_applications(const std::string& customer_id, int limit) override {
        return call("list_applications",
                    [&] { return store_->list_applications(customer_id, limit); });
    }
    control::Result<control::Application> get_application(const std::string& id) override {
        return call("get_application", [&] { return store_->get_application(id); });
    }
    control::Result<control::Application>
    get_application_by_pool(const std::string& pool_id) override {
        return call("get_application_by_pool",
                    [&] { return store_->get_application_by_pool(pool_id); });
    }
    control::Result<control::Application>
    get_application_by_domain(const std::string& domain) override {
        return call("get_application_by_domain",
                    [&] { return store_->get_application_by_domain(domain); });
    }
    control::Result<std::string>
    encrypt_application_credential(const std::string& purpose,
                                   const std::string& plaintext) override {
        return call("encrypt_application_credential",
                    [&] { return store_->encrypt_application_credential(purpose, plaintext); });
    }
    control::Result<std::string>
    decrypt_application_credential(const std::string& purpose,
                                   const std::string& envelope) override {
        return call("decrypt_application_credential",
                    [&] { return store_->decrypt_application_credential(purpose, envelope); });
    }
    control::Result<int> revoke_application_user_sessions(const std::string& application_id,
                                                          const std::string& user_id) override {
        return call("revoke_application_user_sessions", [&] {
            return store_->revoke_application_user_sessions(application_id, user_id);
        });
    }
    control::Result<void> delete_application_user_credentials(const std::string& application_id,
                                                              const std::string& user_id) override {
        return call("delete_application_user_credentials", [&] {
            return store_->delete_application_user_credentials(application_id, user_id);
        });
    }
    control::Result<control::DriveShareLinkRoute>
    drive_share_link_route(const std::string& token_hash) override {
        return call("drive_share_link_route",
                    [&] { return store_->drive_share_link_route(token_hash); });
    }
    audit::Result<audit::Identity> current_lake_identity() override {
        return call("current_lake_identity", [&] { return store_->current_lake_identity(); });
    }
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override {
        return call("lake_identity_by_taap_id",
                    [&] { return store_->lake_identity_by_taap_id(taap_id); });
    }
    audit::Result<void> create_lake_identity(const audit::Identity& identity) override {
        return call("create_lake_identity", [&] { return store_->create_lake_identity(identity); });
    }
    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override {
        return call("rotate_lake_identity",
                    [&] { return store_->rotate_lake_identity(previous, replacement); });
    }
    control::Result<void> append_audit_event(const audit::Event& event) override {
        return call("append_audit_event", [&] { return store_->append_audit_event(event); });
    }
    control::Result<audit::Event> latest_audit_event() override {
        return call("latest_audit_event", [&] { return store_->latest_audit_event(); });
    }
    control::Result<std::vector<audit::Event>>
    list_audit_events(const audit::Filter& filter) override {
        return call("list_audit_events", [&] { return store_->list_audit_events(filter); });
    }
    control::Result<void> set_audit_retention_days(const std::string& customer_id,
                                                   int days) override {
        return call("set_audit_retention_days",
                    [&] { return store_->set_audit_retention_days(customer_id, days); });
    }
    control::Result<std::optional<std::uint64_t>>
    storage_quota(const std::string& customer_id) override {
        return call("storage_quota", [&] { return store_->storage_quota(customer_id); });
    }
    control::Result<std::vector<control::Workspace>>
    list_workspaces(const std::string& customer_id) override {
        return call("list_workspaces", [&] { return store_->list_workspaces(customer_id); });
    }
    control::Result<control::Workspace> get_workspace(const std::string& id) override {
        return call("get_workspace", [&] { return store_->get_workspace(id); });
    }
    control::Result<control::Workspace>
    create_workspace(const control::NewWorkspace& value) override {
        return call("create_workspace", [&] { return store_->create_workspace(value); });
    }
    control::Result<control::Workspace>
    update_workspace(const std::string& id, const control::WorkspacePatch& patch) override {
        return call("update_workspace", [&] { return store_->update_workspace(id, patch); });
    }
    control::Result<control::Workspace> mark_workspace_store_moved(const std::string& id) override {
        return call("mark_workspace_store_moved",
                    [&] { return store_->mark_workspace_store_moved(id); });
    }
    control::Result<control::WorkspaceDeletion>
    delete_workspace_cascade(const control::WorkspaceDeletionRequest& request) override {
        return call("delete_workspace_cascade",
                    [&] { return store_->delete_workspace_cascade(request); });
    }
    control::Result<control::WorkspaceDeletion>
    get_workspace_deletion(const std::string& workspace_id) override {
        return call("get_workspace_deletion",
                    [&] { return store_->get_workspace_deletion(workspace_id); });
    }
    control::Result<std::vector<control::WorkspaceDeletion>>
    pending_workspace_deletions(int limit) override {
        return call("pending_workspace_deletions",
                    [&] { return store_->pending_workspace_deletions(limit); });
    }
    control::Result<void> complete_workspace_deletion(const std::string& workspace_id) override {
        return call("complete_workspace_deletion",
                    [&] { return store_->complete_workspace_deletion(workspace_id); });
    }
    control::Result<control::OciRepositoryRoute>
    create_oci_repository_route(const std::string& customer_id, const std::string& name,
                                const std::string& workspace_id) override {
        return call("create_oci_repository_route", [&] {
            return store_->create_oci_repository_route(customer_id, name, workspace_id);
        });
    }
    control::Result<control::OciRepositoryRoute>
    get_oci_repository_route(const std::string& customer_id, const std::string& name) override {
        return call("get_oci_repository_route",
                    [&] { return store_->get_oci_repository_route(customer_id, name); });
    }
    control::Result<std::vector<control::OciRepositoryRoute>>
    list_oci_repository_routes(const std::string& customer_id,
                               const std::string& workspace_id) override {
        return call("list_oci_repository_routes",
                    [&] { return store_->list_oci_repository_routes(customer_id, workspace_id); });
    }
    control::Result<std::vector<control::OciRepositoryRoute>>
    delete_oci_repository_route(const std::string& customer_id, const std::string& name,
                                const std::string& to_workspace_id) override {
        return call("delete_oci_repository_route", [&] {
            return store_->delete_oci_repository_route(customer_id, name, to_workspace_id);
        });
    }
    control::Result<control::Workspace> set_workspace_storage(const std::string& id,
                                                              std::int32_t storage_gi) override {
        return call("set_workspace_storage",
                    [&] { return store_->set_workspace_storage(id, storage_gi); });
    }
    control::Result<control::VolumeSnapshotPolicy>
    request_volume_snapshot(const control::VolumeSnapshotRequest& request) override {
        return call("request_volume_snapshot",
                    [&] { return store_->request_volume_snapshot(request); });
    }
    control::Result<std::vector<control::VolumeSnapshotPolicy>> volume_snapshots() override {
        return call("volume_snapshots", [&] { return store_->volume_snapshots(); });
    }
    control::Result<std::vector<control::ManagedService>>
    list_services(const std::string& customer_id) override {
        return call("list_services", [&] { return store_->list_services(customer_id); });
    }
    control::Result<control::ManagedService> get_service(const std::string& id) override {
        return call("get_service", [&] { return store_->get_service(id); });
    }
    control::Result<control::ManagedService>
    create_service(const control::NewManagedService& value) override {
        return call("create_service", [&] { return store_->create_service(value); });
    }
    control::Result<control::ManagedService> withdraw_service(const std::string& id) override {
        return call("withdraw_service", [&] { return store_->withdraw_service(id); });
    }
    control::Result<control::ManagedService>
    set_service_stores(const std::string& service_id,
                       const std::vector<control::StoreMountRequest>& mounts) override {
        return call("set_service_stores",
                    [&] { return store_->set_service_stores(service_id, mounts); });
    }
    control::Result<std::vector<control::StoreMount>>
    list_store_mounts(const std::string& customer_id,
                      const std::string& workspace_id = {}) override {
        return call("list_store_mounts",
                    [&] { return store_->list_store_mounts(customer_id, workspace_id); });
    }
    control::Result<std::vector<control::WebApp>>
    list_web_apps(const std::string& customer_id) override {
        return call("list_web_apps", [&] { return store_->list_web_apps(customer_id); });
    }
    control::Result<control::WebApp> get_web_app(const std::string& id) override {
        return call("get_web_app", [&] { return store_->get_web_app(id); });
    }
    control::Result<control::WebApp>
    resolve_web_app_auth_host(const std::string& host, const std::string& workspace_id) override {
        return call("resolve_web_app_auth_host",
                    [&] { return store_->resolve_web_app_auth_host(host, workspace_id); });
    }
    control::Result<control::WebApp> authorize_web_app(const std::string& app_id,
                                                       const std::string& user_id) override {
        return call("authorize_web_app",
                    [&] { return store_->authorize_web_app(app_id, user_id); });
    }
    control::Result<control::WebApp>
    authorize_web_app_session(const std::string& host,
                              const std::string& workspace_id,
                              const std::string& app_id,
                              const std::string& user_id) override {
        return call("authorize_web_app_session", [&] {
            return store_->authorize_web_app_session(host, workspace_id, app_id, user_id);
        });
    }
    control::Result<void>
    create_web_app_authorization_code(const control::WebAppAuthorizationCode& value) override {
        return call("create_web_app_authorization_code",
                    [&] { return store_->create_web_app_authorization_code(value); });
    }
    control::Result<control::WebAppAuthorization>
    consume_web_app_authorization_code(const std::string& host,
                                       const std::string& workspace_id,
                                       const std::string& code_hash) override {
        return call("consume_web_app_authorization_code", [&] {
            return store_->consume_web_app_authorization_code(host, workspace_id, code_hash);
        });
    }
    control::Result<void> sweep_web_app_authorization_codes() override {
        return call("sweep_web_app_authorization_codes",
                    [&] { return store_->sweep_web_app_authorization_codes(); });
    }
    control::Result<control::WebApp> create_web_app(const control::NewWebApp& value) override {
        return call("create_web_app", [&] { return store_->create_web_app(value); });
    }
    control::Result<control::WebApp> update_web_app(const std::string& id,
                                                    const control::WebAppPatch& patch) override {
        return call("update_web_app", [&] { return store_->update_web_app(id, patch); });
    }
    control::Result<void> purge_deleted_web_apps() override {
        return call("purge_deleted_web_apps", [&] { return store_->purge_deleted_web_apps(); });
    }
    control::Result<std::vector<control::Build>>
    list_builds(const std::string& customer_id) override {
        return call("list_builds", [&] { return store_->list_builds(customer_id); });
    }
    control::Result<std::vector<control::AgentTaskStreamScope>>
    list_agent_task_stream_scopes(const std::string& customer_id) override {
        return call("list_agent_task_stream_scopes", [&] {
            return store_->list_agent_task_stream_scopes(customer_id);
        });
    }
    control::Result<control::Build> get_build(const std::string& id) override {
        return call("get_build", [&] { return store_->get_build(id); });
    }
    control::Result<control::Build> create_build(const control::NewBuild& value) override {
        return call("create_build", [&] { return store_->create_build(value); });
    }
    control::Result<control::Build> cancel_build(const std::string& id) override {
        return call("cancel_build", [&] { return store_->cancel_build(id); });
    }
    control::Result<std::vector<control::ManagedStore>>
    list_stores(const std::string& customer_id, const std::string& workspace_id = {}) override {
        return call("list_stores", [&] { return store_->list_stores(customer_id, workspace_id); });
    }
    control::Result<control::ManagedStore> get_store(const std::string& id) override {
        return call("get_store", [&] { return store_->get_store(id); });
    }
    control::Result<control::ManagedStore>
    create_store(const control::NewManagedStore& value) override {
        return call("create_store", [&] { return store_->create_store(value); });
    }
    control::Result<control::ManagedStore> withdraw_store(const std::string& id) override {
        return call("withdraw_store", [&] { return store_->withdraw_store(id); });
    }
    control::Result<std::vector<control::UsageLimit>>
    usage_limits(const std::string& customer_id) override {
        return call("usage_limits", [&] { return store_->usage_limits(customer_id); });
    }
    control::Result<control::UsageLimit>
    set_usage_limit(const control::UsageLimit& limit) override {
        return call("set_usage_limit", [&] { return store_->set_usage_limit(limit); });
    }
    control::Result<std::vector<control::BillingPlan>> billing_plans() override {
        return call("billing_plans", [&] { return store_->billing_plans(); });
    }
    control::Result<std::vector<control::WorkloadResources>> workload_resources() override {
        return call("workload_resources", [&] { return store_->workload_resources(); });
    }
    control::Result<void> put_workload_resources(const control::WorkloadResources& row) override {
        return call("put_workload_resources", [&] { return store_->put_workload_resources(row); });
    }
    control::Result<void>
    put_mcp_connection_dispatch(const control::McpConnectionDispatch& row) override {
        return call("put_mcp_connection_dispatch",
                    [&] { return store_->put_mcp_connection_dispatch(row); });
    }
    control::Result<void> remove_mcp_connection_dispatch(const std::string& workspace_id,
                                                         const std::string& connection_id) override {
        return call("remove_mcp_connection_dispatch", [&] {
            return store_->remove_mcp_connection_dispatch(workspace_id, connection_id);
        });
    }
    control::Result<std::vector<control::McpConnectionDispatch>>
    mcp_stream_dispatch(int limit) override {
        return call("mcp_stream_dispatch", [&] { return store_->mcp_stream_dispatch(limit); });
    }
    control::Result<std::vector<control::McpConnectionDispatch>>
    mcp_exported_dispatch(const std::string& workspace_id) override {
        return call("mcp_exported_dispatch",
                    [&] { return store_->mcp_exported_dispatch(workspace_id); });
    }
    control::Result<std::vector<control::AppTransfer>>
    app_transfer(const std::string& since_hour) override {
        return call("app_transfer", [&] { return store_->app_transfer(since_hour); });
    }
    control::Result<void> add_app_transfer(const control::AppTransfer& row) override {
        return call("add_app_transfer", [&] { return store_->add_app_transfer(row); });
    }
    control::Result<control::BillingPlan>
    put_billing_plan(const control::BillingPlan& plan) override {
        return call("put_billing_plan", [&] { return store_->put_billing_plan(plan); });
    }
    control::Result<control::Invoice> create_invoice(const control::Invoice& invoice) override {
        return call("create_invoice", [&] { return store_->create_invoice(invoice); });
    }
    control::Result<std::vector<control::Invoice>>
    list_invoices(const std::string& customer_id) override {
        return call("list_invoices", [&] { return store_->list_invoices(customer_id); });
    }
    control::Result<control::Invoice> get_invoice(const std::string& id) override {
        return call("get_invoice", [&] { return store_->get_invoice(id); });
    }
    control::Result<control::Invoice> set_invoice_state(const std::string& id,
                                                        const std::string& state,
                                                        const std::string& provider_reference,
                                                        const std::string& note) override {
        return call("set_invoice_state",
                    [&] { return store_->set_invoice_state(id, state, provider_reference, note); });
    }
    control::Result<control::Invoice> set_invoice_provider_status(const std::string& id,
                                                                  const std::string& from,
                                                                  const std::string& to) override {
        return call("set_invoice_provider_status",
                    [&] { return store_->set_invoice_provider_status(id, from, to); });
    }
    control::Result<std::string> billing_provider_customer(const std::string& customer_id,
                                                           const std::string& provider) override {
        return call("billing_provider_customer",
                    [&] { return store_->billing_provider_customer(customer_id, provider); });
    }
    control::Result<std::string>
    set_billing_provider_customer(const std::string& customer_id,
                                  const std::string& provider,
                                  const std::string& provider_customer_id) override {
        return call("set_billing_provider_customer", [&] {
            return store_->set_billing_provider_customer(
                customer_id, provider, provider_customer_id);
        });
    }
    control::Result<std::string>
    replace_billing_provider_customer(const std::string& customer_id,
                                      const std::string& provider,
                                      const std::string& expected_id,
                                      const std::string& provider_customer_id) override {
        return call("replace_billing_provider_customer", [&] {
            return store_->replace_billing_provider_customer(
                customer_id, provider, expected_id, provider_customer_id);
        });
    }
    control::Result<control::PaymentEventResult>
    apply_payment_event(const control::PaymentEvent& event) override {
        return call("apply_payment_event", [&] { return store_->apply_payment_event(event); });
    }
    control::Result<std::optional<control::BillingContact>>
    billing_contact(const std::string& customer_id) override {
        return call("billing_contact", [&] { return store_->billing_contact(customer_id); });
    }
    control::Result<control::BillingContact>
    set_billing_contact(const control::BillingContact& contact) override {
        return call("set_billing_contact", [&] { return store_->set_billing_contact(contact); });
    }
    control::Result<control::AccountBalance>
    account_balance(const std::string& customer_id) override {
        return call("account_balance", [&] { return store_->account_balance(customer_id); });
    }
    control::Result<control::AccountBalance>
    update_account_balance(const control::AccountBalance& next,
                           std::int64_t expected_version) override {
        return call("update_account_balance",
                    [&] { return store_->update_account_balance(next, expected_version); });
    }
    control::Result<control::BalanceCreditResult>
    apply_balance_credit(const control::BalanceCredit& credit) override {
        return call("apply_balance_credit", [&] { return store_->apply_balance_credit(credit); });
    }
    control::Result<void> set_balance_charge(const control::BalanceCharge& charge) override {
        return call("set_balance_charge", [&] { return store_->set_balance_charge(charge); });
    }
    control::Result<std::vector<control::BalanceCredit>>
    balance_credits(const std::string& customer_id, int limit) override {
        return call("balance_credits", [&] { return store_->balance_credits(customer_id, limit); });
    }
    control::Result<std::vector<control::BalanceCharge>>
    balance_charges(const std::string& customer_id) override {
        return call("balance_charges", [&] { return store_->balance_charges(customer_id); });
    }
    control::Result<std::vector<std::string>> balance_customers() override {
        return call("balance_customers", [&] { return store_->balance_customers(); });
    }
    control::Result<control::BalanceCredit> balance_credit(const std::string& credit_id) override {
        return call("balance_credit", [&] { return store_->balance_credit(credit_id); });
    }
    control::Result<std::vector<control::BalanceCredit>>
    end_balance_grants(const std::string& customer_id,
                       const std::string& at,
                       const std::string& revoke_id) override {
        return call("end_balance_grants",
                    [&] { return store_->end_balance_grants(customer_id, at, revoke_id); });
    }
    control::Result<control::Customer>
    personal_account_by_email(const std::string& email) override {
        return call("personal_account_by_email",
                    [&] { return store_->personal_account_by_email(email); });
    }
    control::Result<control::Coupon> create_coupon(const control::Coupon& coupon) override {
        return call("create_coupon", [&] { return store_->create_coupon(coupon); });
    }
    control::Result<control::Coupon> coupon_by_code(const std::string& code) override {
        return call("coupon_by_code", [&] { return store_->coupon_by_code(code); });
    }
    control::Result<control::Coupon> get_coupon(const std::string& coupon_id) override {
        return call("get_coupon", [&] { return store_->get_coupon(coupon_id); });
    }
    control::Result<std::vector<control::CouponReport>>
    coupon_reports(const std::string& campaign) override {
        return call("coupon_reports", [&] { return store_->coupon_reports(campaign); });
    }
    control::Result<control::Coupon> disable_coupon(const std::string& coupon_id,
                                                    const std::string& by) override {
        return call("disable_coupon", [&] { return store_->disable_coupon(coupon_id, by); });
    }
    control::Result<control::CouponRedemption>
    coupon_redemption(const std::string& coupon_id, const std::string& customer_id) override {
        return call("coupon_redemption",
                    [&] { return store_->coupon_redemption(coupon_id, customer_id); });
    }
    control::Result<control::CouponClaim>
    claim_coupon(const control::CouponRedemption& redemption) override {
        return call("claim_coupon", [&] { return store_->claim_coupon(redemption); });
    }
    control::Result<void> finish_coupon_redemption(const std::string& redemption_id,
                                                   const std::string& credit_id) override {
        return call("finish_coupon_redemption",
                    [&] { return store_->finish_coupon_redemption(redemption_id, credit_id); });
    }
    control::Result<bool> coupon_attempts_exhausted(const std::vector<std::string>& keys,
                                                    int limit,
                                                    int window_seconds) override {
        return call("coupon_attempts_exhausted",
                    [&] { return store_->coupon_attempts_exhausted(keys, limit, window_seconds); });
    }
    control::Result<void> coupon_attempt_failed(const std::vector<std::string>& keys,
                                                int window_seconds) override {
        return call("coupon_attempt_failed",
                    [&] { return store_->coupon_attempt_failed(keys, window_seconds); });
    }
    control::Result<control::CloudOrder>
    create_cloud_order(const control::NewCloudOrder& order) override {
        return call("create_cloud_order", [&] { return store_->create_cloud_order(order); });
    }
    control::Result<control::CloudOrder> get_cloud_order(const std::string& id) override {
        return call("get_cloud_order", [&] { return store_->get_cloud_order(id); });
    }
    control::Result<control::CloudOrder>
    transition_cloud_order(const control::CloudOrderTransition& change) override {
        return call("transition_cloud_order",
                    [&] { return store_->transition_cloud_order(change); });
    }
    control::Result<control::CloudOrderPaymentResult>
    apply_cloud_order_payment(const control::CloudOrderPayment& payment) override {
        return call("apply_cloud_order_payment",
                    [&] { return store_->apply_cloud_order_payment(payment); });
    }
    control::Result<control::CloudOrderPaymentResult>
    apply_cloud_order_subscription(const control::CloudOrderSubscriptionEvent& event) override {
        return call("apply_cloud_order_subscription",
                    [&] { return store_->apply_cloud_order_subscription(event); });
    }
    control::Result<std::vector<control::CloudOrder>>
    claim_cloud_order_emails(int limit, int lease_seconds) override {
        return call("claim_cloud_order_emails",
                    [&] { return store_->claim_cloud_order_emails(limit, lease_seconds); });
    }
    control::Result<void>
    record_cloud_order_email(const control::CloudOrderEmailOutcome& outcome) override {
        return call("record_cloud_order_email",
                    [&] { return store_->record_cloud_order_email(outcome); });
    }
    control::Result<std::vector<control::CloudOrderEvent>>
    unaudited_cloud_order_events(int limit) override {
        return call("unaudited_cloud_order_events",
                    [&] { return store_->unaudited_cloud_order_events(limit); });
    }
    control::Result<void>
    mark_cloud_order_events_audited(const std::vector<std::int64_t>& ids) override {
        return call("mark_cloud_order_events_audited",
                    [&] { return store_->mark_cloud_order_events_audited(ids); });
    }
    control::Result<std::uint64_t> set_storage_quota(const std::string& customer_id,
                                                     std::uint64_t quota_bytes) override {
        return call("set_storage_quota",
                    [&] { return store_->set_storage_quota(customer_id, quota_bytes); });
    }
    control::Result<control::PkiAuthorityRecord>
    create_pki_authority(const control::NewPkiAuthorityRecord& value,
                         const std::string& instance_kek) override {
        return call("create_pki_authority",
                    [&] { return store_->create_pki_authority(value, instance_kek); });
    }
    control::Result<control::PkiAuthorityMaterial>
    get_pki_authority(const std::string& customer_id, const std::string& instance_kek) override {
        return call("get_pki_authority",
                    [&] { return store_->get_pki_authority(customer_id, instance_kek); });
    }
    control::Result<control::PkiCallerRecord>
    create_pki_caller(const control::NewPkiCallerRecord& value) override {
        return call("create_pki_caller", [&] { return store_->create_pki_caller(value); });
    }
    control::Result<std::vector<control::PkiCallerRecord>>
    list_pki_callers(const std::string& customer_id, int limit) override {
        return call("list_pki_callers",
                    [&] { return store_->list_pki_callers(customer_id, limit); });
    }
    control::Result<control::PkiCertificateRecord>
    create_pki_certificate(const control::NewPkiCertificateRecord& value) override {
        return call("create_pki_certificate",
                    [&] { return store_->create_pki_certificate(value); });
    }
    control::Result<control::PkiCertificateRecord>
    get_pki_certificate(const std::string& id) override {
        return call("get_pki_certificate", [&] { return store_->get_pki_certificate(id); });
    }
    control::Result<control::PkiCertificateRecord>
    revoke_pki_certificate(const std::string& id) override {
        return call("revoke_pki_certificate", [&] { return store_->revoke_pki_certificate(id); });
    }
    control::Result<bool> has_active_agent_certificate(const std::string& customer_id,
                                                       const std::string& taap_id,
                                                       const std::string& at) override {
        return call("has_active_agent_certificate",
                    [&] { return store_->has_active_agent_certificate(customer_id, taap_id, at); });
    }
    control::Result<control::AgentSession>
    create_agent_session(const control::NewAgentSession& value) override {
        return call("create_agent_session", [&] { return store_->create_agent_session(value); });
    }
    control::Result<control::AgentLaunch>
    create_agent_launch(const control::NewAgentLaunch& value) override {
        return call("create_agent_launch", [&] { return store_->create_agent_launch(value); });
    }
    control::Result<control::AgentLaunch> get_agent_launch(const std::string& id) override {
        return call("get_agent_launch", [&] { return store_->get_agent_launch(id); });
    }
    control::Result<std::vector<control::AgentLaunch>>
    list_agent_launches(const std::string& customer_id, const std::string& agent_id, int limit,
                        const std::string& workspace_id = {}) override {
        return call("list_agent_launches", [&] {
            return store_->list_agent_launches(customer_id, agent_id, limit, workspace_id);
        });
    }
    control::Result<control::AgentLaunch> withdraw_agent_launch(const std::string& id) override {
        return call("withdraw_agent_launch", [&] { return store_->withdraw_agent_launch(id); });
    }
    control::Result<std::size_t> expire_unstarted_agent_launches(
        std::int64_t seconds, const std::string& reason) override {
        return call("expire_unstarted_agent_launches",
                    [&] { return store_->expire_unstarted_agent_launches(seconds, reason); });
    }
    control::Result<control::AgentWorkloadsWithdrawn>
    withdraw_agent_workloads(const std::string& customer_id,
                             const std::string& workspace_id,
                             const std::string& agent_id,
                             const std::string& reason) override {
        return call("withdraw_agent_workloads", [&] {
            return store_->withdraw_agent_workloads(customer_id, workspace_id, agent_id, reason);
        });
    }
    control::Result<control::AgentTask>
    create_agent_task(const control::AgentTask& value) override {
        return call("create_agent_task", [&] { return store_->create_agent_task(value); });
    }
    control::Result<control::AgentTask> get_agent_task(const std::string& id) override {
        return call("get_agent_task", [&] { return store_->get_agent_task(id); });
    }
    control::Result<control::AgentTask> latest_agent_task(const std::string& customer_id,
                                                          const std::string& kind,
                                                          const std::string& workspace_id) override {
        return call("latest_agent_task",
                    [&] {
                        return store_->latest_agent_task(customer_id, kind, workspace_id);
                    });
    }
    control::Result<control::AgentTask> latest_agent_conversation_task(
        const std::string& customer_id,
        const std::string& conversation_id,
        const std::string& agent,
        const std::string& kind,
        const std::string& workspace_id) override {
        return call("latest_agent_conversation_task", [&] {
            return store_->latest_agent_conversation_task(
                customer_id, conversation_id, agent, kind, workspace_id);
        });
    }
    control::Result<std::vector<control::AgentTask>>
    list_conversation_agent_tasks(const std::string& customer_id,
                                  const std::string& workspace_id,
                                  const std::string& conversation_id,
                                  const std::string& requested_by_user_id) override {
        return call("list_conversation_agent_tasks", [&] {
            return store_->list_conversation_agent_tasks(
                customer_id, workspace_id, conversation_id, requested_by_user_id);
        });
    }
    control::Result<std::size_t> delete_agent_tasks(const std::string& customer_id,
                                                    const std::vector<std::string>& ids) override {
        return call("delete_agent_tasks",
                    [&] { return store_->delete_agent_tasks(customer_id, ids); });
    }
    control::Result<control::AgentTask> claim_agent_task(const std::string& id,
                                                         const std::string& agent,
                                                         const std::string& sha256,
                                                         std::uint64_t events_from,
                                                         int lease_seconds) override {
        return call("claim_agent_task", [&] {
            return store_->claim_agent_task(id, agent, sha256, events_from, lease_seconds);
        });
    }
    control::Result<void> renew_agent_task_lease(const std::string& id,
                                                 const std::string& sha256) override {
        return call("renew_agent_task_lease", [&]() -> control::Result<void> {
            return store_->renew_agent_task_lease(id, sha256);
        });
    }
    control::Result<void> mark_agent_task_notice_published(const std::string& id,
                                                           int attempts,
                                                           const std::string& phase) override {
        return call("mark_agent_task_notice_published", [&]() -> control::Result<void> {
            return store_->mark_agent_task_notice_published(id, attempts, phase);
        });
    }
    control::Result<control::AgentTask>
    record_agent_task_event(const std::string& id,
                            const std::string& agent,
                            const std::string& phase) override {
        return call("record_agent_task_event",
                    [&] { return store_->record_agent_task_event(id, agent, phase); });
    }
    control::Result<void> fail_agent_task(const std::string& id,
                                          const std::string& notice) override {
        return call("fail_agent_task", [&] { return store_->fail_agent_task(id, notice); });
    }
    control::Result<void> mark_agent_task_published(const std::string& id) override {
        return call("mark_agent_task_published",
                    [&] { return store_->mark_agent_task_published(id); });
    }
    control::Result<control::StaleAgentTasks>
    stale_agent_tasks(std::chrono::seconds republish_after) override {
        return call("stale_agent_tasks",
                    [&] { return store_->stale_agent_tasks(republish_after); });
    }
    control::Result<control::AgentCronSchedule>
    create_agent_cron_schedule(const control::NewAgentCronSchedule& value) override {
        return call("create_agent_cron_schedule",
                    [&] { return store_->create_agent_cron_schedule(value); });
    }
    control::Result<control::AgentCronSchedule>
    get_agent_cron_schedule(const std::string& id) override {
        return call("get_agent_cron_schedule", [&] { return store_->get_agent_cron_schedule(id); });
    }
    control::Result<std::vector<control::AgentCronSchedule>>
    list_agent_cron_schedules(const std::string& customer_id, const std::string& agent_id,
                              const std::string& workspace_id = {}) override {
        return call("list_agent_cron_schedules", [&] {
            return store_->list_agent_cron_schedules(customer_id, agent_id, workspace_id);
        });
    }
    control::Result<control::AgentCronSchedule>
    withdraw_agent_cron_schedule(const std::string& id) override {
        return call("withdraw_agent_cron_schedule",
                    [&] { return store_->withdraw_agent_cron_schedule(id); });
    }
    control::Result<control::MethodologyRun>
    create_methodology_run(const control::NewMethodologyRun& value) override {
        return call("create_methodology_run",
                    [&] { return store_->create_methodology_run(value); });
    }
    control::Result<control::MethodologyRun> get_methodology_run(const std::string& id) override {
        return call("get_methodology_run", [&] { return store_->get_methodology_run(id); });
    }
    control::Result<std::vector<control::MethodologyRun>>
    list_methodology_runs(const std::string& customer_id,
                          const std::string& workspace_id,
                          const std::string& methodology_id,
                          int limit) override {
        return call("list_methodology_runs", [&] {
            return store_->list_methodology_runs(customer_id, workspace_id, methodology_id, limit);
        });
    }
    control::Result<control::MethodologyRun>
    withdraw_methodology_run(const std::string& id, const std::string& reason) override {
        return call("withdraw_methodology_run",
                    [&] { return store_->withdraw_methodology_run(id, reason); });
    }
    control::Result<std::size_t>
    expire_unstarted_methodology_runs(std::int64_t seconds, const std::string& reason) override {
        return call("expire_unstarted_methodology_runs",
                    [&] { return store_->expire_unstarted_methodology_runs(seconds, reason); });
    }
    control::Result<void>
    put_methodology_run_status(const std::string& id,
                               const control::MethodologyRunStatus& status) override {
        return call("put_methodology_run_status",
                    [&] { return store_->put_methodology_run_status(id, status); });
    }
    control::Result<control::MethodologyDeployment>
    create_methodology_deployment(const control::NewMethodologyDeployment& in) override {
        return call("create_methodology_deployment",
                    [&] { return store_->create_methodology_deployment(in); });
    }
    control::Result<control::MethodologyDeployment> get_methodology_deployment(const std::string& id) override {
        return call("get_methodology_deployment",
                    [&] { return store_->get_methodology_deployment(id); });
    }
    control::Result<std::vector<control::MethodologyDeployment>>
    list_methodology_deployments(const std::string& customer_id,
                                 const std::string& workspace_id,
                                 const std::string& model_id,
                                 int limit) override {
        return call("list_methodology_deployments", [&] {
            return store_->list_methodology_deployments(customer_id, workspace_id, model_id, limit);
        });
    }
    control::Result<control::MethodologyDeployment>
    withdraw_methodology_deployment(const std::string& id, const std::string& reason) override {
        return call("withdraw_methodology_deployment",
                    [&] { return store_->withdraw_methodology_deployment(id, reason); });
    }
    control::Result<void>
    put_methodology_deployment_status(const std::string& id,
                                      const control::MethodologyDeploymentStatus& status) override {
        return call("put_methodology_deployment_status",
                    [&] { return store_->put_methodology_deployment_status(id, status); });
    }
    control::Result<std::vector<control::AgentLaunch>>
    list_agent_cron_runs(const std::string& customer_id, const std::string& agent_id, int limit,
                         const std::string& workspace_id = {}) override {
        return call("list_agent_cron_runs", [&] {
            return store_->list_agent_cron_runs(customer_id, agent_id, limit, workspace_id);
        });
    }
    control::Result<control::AgentSession> revoke_agent_session(const std::string& customer_id,
                                                                const std::string& id) override {
        return call("revoke_agent_session",
                    [&] { return store_->revoke_agent_session(customer_id, id); });
    }
    control::Result<bool>
    customer_allows_non_expiring_credentials(const std::string& customer_id) override {
        return call("customer_allows_non_expiring_credentials",
                    [&] { return store_->customer_allows_non_expiring_credentials(customer_id); });
    }
    control::Result<void> set_customer_non_expiring_credentials(const std::string& customer_id,
                                                                bool allowed) override {
        return call("set_customer_non_expiring_credentials", [&] {
            return store_->set_customer_non_expiring_credentials(customer_id, allowed);
        });
    }
    control::Result<control::AgentSession> get_agent_session(const std::string& customer_id,
                                                             const std::string& id) override {
        return call("get_agent_session",
                    [&] { return store_->get_agent_session(customer_id, id); });
    }
    control::Result<control::PkiRecoveryCeremonyRecord>
    create_pki_recovery_ceremony(const control::NewPkiRecoveryCeremonyRecord& value) override {
        return call("create_pki_recovery_ceremony",
                    [&] { return store_->create_pki_recovery_ceremony(value); });
    }
    control::Result<control::PkiRecoveryCeremonyRecord>
    get_pki_recovery_ceremony(const std::string& customer_id,
                              const std::string& ceremony_id) override {
        return call("get_pki_recovery_ceremony",
                    [&] { return store_->get_pki_recovery_ceremony(customer_id, ceremony_id); });
    }
    control::Result<int> claim_pki_recovery_share(const std::string& customer_id,
                                                  const std::string& ceremony_id,
                                                  const std::string& principal) override {
        return call("claim_pki_recovery_share", [&] {
            return store_->claim_pki_recovery_share(customer_id, ceremony_id, principal);
        });
    }
    control::Result<control::PkiRecoveryCeremonyRecord>
    submit_pki_recovery_share(const std::string& customer_id,
                              const std::string& ceremony_id,
                              const std::string& principal,
                              const control::PkiRecoveryShareEvidence& evidence) override {
        return call("submit_pki_recovery_share", [&] {
            return store_->submit_pki_recovery_share(customer_id, ceremony_id, principal, evidence);
        });
    }
    control::Result<control::SecretMountRoute>
    create_secret_mount_route(const control::SecretMountRoute& value) override {
        return call("create_secret_mount_route",
                    [&] { return store_->create_secret_mount_route(value); });
    }
    control::Result<control::SecretMountRoute>
    get_secret_mount_route(const std::string& mount_id) override {
        return call("get_secret_mount_route",
                    [&] { return store_->get_secret_mount_route(mount_id); });
    }
    control::Result<void> delete_secret_mount_route(const std::string& mount_id) override {
        return call("delete_secret_mount_route",
                    [&] { return store_->delete_secret_mount_route(mount_id); });
    }
    control::Result<std::vector<control::SecretMountRoute>>
    list_secret_mount_routes(const std::string& customer_id,
                             const std::string& workspace_id) override {
        return call("list_secret_mount_routes",
                    [&] { return store_->list_secret_mount_routes(customer_id, workspace_id); });
    }
    control::Result<control::WrappedDataKey>
    wrap_secret_data_key(const std::string& customer_id,
                         const std::string& owner_id,
                         std::uint64_t key_version,
                         const std::string& raw_key,
                         const std::string& instance_kek) override {
        return call("wrap_secret_data_key", [&] {
            return store_->wrap_secret_data_key(customer_id, owner_id, key_version, raw_key,
                                                instance_kek);
        });
    }
    control::Result<std::string> unwrap_secret_data_key(const std::string& customer_id,
                                                        const std::string& owner_id,
                                                        std::uint64_t key_version,
                                                        const control::WrappedDataKey& wrapped,
                                                        const std::string& instance_kek) override {
        return call("unwrap_secret_data_key", [&] {
            return store_->unwrap_secret_data_key(customer_id, owner_id, key_version, wrapped,
                                                  instance_kek);
        });
    }
    control::Result<control::CustomerKekState>
    customer_kek_state(const std::string& customer_id) override {
        return call("customer_kek_state", [&] { return store_->customer_kek_state(customer_id); });
    }
    control::Result<control::CustomerKekRotation>
    rotate_customer_kek(const std::string& customer_id, const std::string& instance_kek) override {
        return call("rotate_customer_kek",
                    [&] { return store_->rotate_customer_kek(customer_id, instance_kek); });
    }
    control::Result<void> retire_customer_kek(const std::string& customer_id,
                                              std::uint64_t previous_version) override {
        return call("retire_customer_kek",
                    [&] { return store_->retire_customer_kek(customer_id, previous_version); });
    }
    control::Result<control::SecretLeaseRoute>
    record_secret_lease_route(const control::SecretLeaseRoute& value) override {
        return call("record_secret_lease_route",
                    [&] { return store_->record_secret_lease_route(value); });
    }
    control::Result<control::SecretLeaseRoute>
    get_secret_lease_route(const std::string& lease_id) override {
        return call("get_secret_lease_route",
                    [&] { return store_->get_secret_lease_route(lease_id); });
    }
    control::Result<void> reschedule_secret_lease(const std::string& lease_id,
                                                  std::int64_t due_us) override {
        return call("reschedule_secret_lease",
                    [&] { return store_->reschedule_secret_lease(lease_id, due_us); });
    }
    control::Result<void> delete_secret_lease_route(const std::string& lease_id) override {
        return call("delete_secret_lease_route",
                    [&] { return store_->delete_secret_lease_route(lease_id); });
    }
    control::Result<std::vector<control::SecretLeaseRoute>>
    claim_due_secret_leases(int limit, int hold_seconds) override {
        return call("claim_due_secret_leases",
                    [&] { return store_->claim_due_secret_leases(limit, hold_seconds); });
    }

    control::Result<control::IssuedSession>
    sign_up_password(const control::NewPasswordUser& value) override {
        return call("sign_up_password", [&] { return store_->sign_up_password(value); });
    }
    control::Result<control::IdentityProofChallenge>
    begin_identity_proof(const std::string& pool_id, const std::string& document) override {
        return call("begin_identity_proof",
                    [&] { return store_->begin_identity_proof(pool_id, document); });
    }
    control::Result<control::AuthenticationAttempt>
    authentication_attempt(const std::string& pool,
                           const std::string& email,
                           const std::string& user,
                           const std::string& client) override {
        return call("authentication_attempt",
                    [&] { return store_->authentication_attempt(pool, email, user, client); });
    }
    control::Result<void>
    authentication_attempt_finished(const control::AuthenticationAttempt& attempt,
                                    control::AuthenticationOutcome outcome) override {
        return call("authentication_attempt_finished",
                    [&] { return store_->authentication_attempt_finished(attempt, outcome); });
    }
    control::Result<control::IssuedSession>
    sign_in_password(const control::PasswordSignIn& value) override {
        return call("sign_in_password", [&] { return store_->sign_in_password(value); });
    }
    control::Result<control::IssuedSession>
    sign_in_taap(const control::TaapSignIn& value) override {
        return call("sign_in_taap", [&] { return store_->sign_in_taap(value); });
    }
    control::Result<control::MfaEnrollment>
    enroll_mfa(const std::string& user_id,
               const std::string& pool_id,
               const control::MfaEnrollmentProof& proof) override {
        return call("enroll_mfa", [&] { return store_->enroll_mfa(user_id, pool_id, proof); });
    }
    control::Result<bool> mfa_enrolled(const std::string& user_id,
                                       const std::string& pool_id) override {
        return call("mfa_enrolled", [&] { return store_->mfa_enrolled(user_id, pool_id); });
    }
    control::Result<void> verify_mfa_code(const std::string& user_id,
                                          const std::string& pool_id,
                                          const std::string& code) override {
        return call("verify_mfa_code",
                    [&] { return store_->verify_mfa_code(user_id, pool_id, code); });
    }
    control::Result<void> disable_mfa(const std::string& user_id,
                                      const std::string& pool_id,
                                      const std::string& code) override {
        return call("disable_mfa", [&] { return store_->disable_mfa(user_id, pool_id, code); });
    }
    control::Result<control::IssuedSession>
    sign_in_totp(const control::PasswordSignIn& value) override {
        return call("sign_in_totp", [&] { return store_->sign_in_totp(value); });
    }
    control::Result<control::WebAuthnChallenge>
    begin_passkey_registration(const std::string& user_id,
                               const std::string& pool_id,
                               const std::string& rp_id,
                               const std::string& origin) override {
        return call("begin_passkey_registration", [&] {
            return store_->begin_passkey_registration(user_id, pool_id, rp_id, origin);
        });
    }
    control::Result<void> register_passkey(const control::PasskeyRegistration& value) override {
        return call("register_passkey", [&] { return store_->register_passkey(value); });
    }
    control::Result<control::WebAuthnChallenge> begin_passkey_sign_in(
        const std::string& pool_id, const std::string& rp_id, const std::string& origin) override {
        return call("begin_passkey_sign_in",
                    [&] { return store_->begin_passkey_sign_in(pool_id, rp_id, origin); });
    }
    control::Result<control::IssuedSession>
    sign_in_passkey(const control::PasskeyAssertion& value) override {
        return call("sign_in_passkey", [&] { return store_->sign_in_passkey(value); });
    }
    control::Result<control::IssuedSession>
    sign_in_oauth(const control::OAuthIdentity& value) override {
        return call("sign_in_oauth", [&] { return store_->sign_in_oauth(value); });
    }
    control::Result<control::IssuedSession>
    sign_in_clerk(const control::ClerkIdentity& value) override {
        return call("sign_in_clerk", [&] { return store_->sign_in_clerk(value); });
    }
    control::Result<control::ClerkUserProvisioned>
    provision_clerk_user(const control::ClerkIdentity& value) override {
        return call("provision_clerk_user", [&] { return store_->provision_clerk_user(value); });
    }
    control::Result<bool> apply_clerk_user_state(const control::ClerkUserState& value) override {
        return call("apply_clerk_user_state",
                    [&] { return store_->apply_clerk_user_state(value); });
    }
    control::Result<void> revoke_clerk_sessions(const std::string& subject,
                                                const std::string& session_id,
                                                std::int64_t revoked_at) override {
        return call("revoke_clerk_sessions", [&] {
            return store_->revoke_clerk_sessions(subject, session_id, revoked_at);
        });
    }
    control::Result<std::string> update_clerk_user(const control::ClerkIdentity& value) override {
        return call("update_clerk_user", [&] { return store_->update_clerk_user(value); });
    }
    control::Result<std::string> disable_clerk_user(const std::string& subject) override {
        return call("disable_clerk_user", [&] { return store_->disable_clerk_user(subject); });
    }
    control::Result<bool> clerk_webhook_applied(const std::string& delivery_id) override {
        return call("clerk_webhook_applied",
                    [&] { return store_->clerk_webhook_applied(delivery_id); });
    }
    control::Result<void> record_clerk_webhook(const std::string& delivery_id,
                                               const std::string& event_type,
                                               const std::string& subject) override {
        return call("record_clerk_webhook",
                    [&] { return store_->record_clerk_webhook(delivery_id, event_type, subject); });
    }
    control::Result<control::AccountIdentity>
    account_identity(const std::string& user_id) override {
        return call("account_identity", [&] { return store_->account_identity(user_id); });
    }
    control::Result<control::ManagedKey> managed_key(const std::string& user_id) override {
        return call("managed_key", [&] { return store_->managed_key(user_id); });
    }
    control::Result<void>
    create_identity_link_challenge(const control::IdentityLinkChallenge& value) override {
        return call("create_identity_link_challenge",
                    [&] { return store_->create_identity_link_challenge(value); });
    }
    control::Result<control::IdentityLinkChallenge>
    consume_identity_link_challenge(const std::string& user_id,
                                    const std::string& challenge) override {
        return call("consume_identity_link_challenge",
                    [&] { return store_->consume_identity_link_challenge(user_id, challenge); });
    }
    control::Result<control::AccountIdentity>
    link_own_identity(const control::IdentityLink& value) override {
        return call("link_own_identity", [&] { return store_->link_own_identity(value); });
    }
    control::Result<std::string> account_for_identity(const std::string& taap_id) override {
        return call("account_for_identity", [&] { return store_->account_for_identity(taap_id); });
    }
    control::Result<std::string> registered_identity_document(const std::string& taap_id) override {
        return call("registered_identity_document",
                    [&] { return store_->registered_identity_document(taap_id); });
    }
    control::Result<void> register_oauth_provider(const control::OAuthProvider& value) override {
        return call("register_oauth_provider",
                    [&] { return store_->register_oauth_provider(value); });
    }
    control::Result<control::OAuthProvider>
    get_oauth_provider(const std::string& pool_id, const std::string& provider) override {
        return call("get_oauth_provider",
                    [&] { return store_->get_oauth_provider(pool_id, provider); });
    }
    control::Result<void> begin_oauth(const control::OAuthFlow& value) override {
        return call("begin_oauth", [&] { return store_->begin_oauth(value); });
    }
    control::Result<control::OAuthFlow> consume_oauth(const std::string& provider,
                                                      const std::string& state,
                                                      const std::string& browser_binding) override {
        return call("consume_oauth",
                    [&] { return store_->consume_oauth(provider, state, browser_binding); });
    }
    control::Result<control::IssuedSession> create_token(const control::NewToken& value) override {
        return call("create_token", [&] { return store_->create_token(value); });
    }
    control::Result<control::IssuedBrowserKey>
    create_browser_key(const control::NewBrowserKey& value) override {
        return call("create_browser_key", [&] { return store_->create_browser_key(value); });
    }
    control::Result<std::vector<control::BrowserKey>>
    list_browser_keys(const std::string& customer_id, const std::string& workspace_id) override {
        return call("list_browser_keys",
                    [&] { return store_->list_browser_keys(customer_id, workspace_id); });
    }
    control::Result<control::BrowserKey>
    revoke_browser_key(const std::string& customer_id,
                       const std::string& id,
                       const std::string& revoked_by) override {
        return call("revoke_browser_key",
                    [&] { return store_->revoke_browser_key(customer_id, id, revoked_by); });
    }
    control::Result<control::BrowserKeyAdmission>
    admit_browser_key_call(const std::string& id) override {
        return call("admit_browser_key_call", [&] { return store_->admit_browser_key_call(id); });
    }
    control::Result<control::DomainClaim> claim_domain(const control::NewDomainClaim& in) override {
        return call("claim_domain", [&] { return store_->claim_domain(in); });
    }
    control::Result<control::DomainClaim> get_domain_claim(const std::string& host) override {
        return call("get_domain_claim", [&] { return store_->get_domain_claim(host); });
    }
    control::Result<control::DomainClaim> verify_domain_claim(const std::string& host,
                                                              const std::string& repository_id,
                                                              const std::string& environment,
                                                              const std::string& token) override {
        return call("verify_domain_claim", [&] {
            return store_->verify_domain_claim(host, repository_id, environment, token);
        });
    }
    control::Result<void> release_domain_claim(const std::string& host,
                                               const std::string& repository_id,
                                               const std::string& environment) override {
        return call("release_domain_claim",
                    [&] { return store_->release_domain_claim(host, repository_id, environment); });
    }
    control::Result<std::vector<control::Session>>
    list_sessions(const std::string& user_id, const std::string& pool_id) override {
        return call("list_sessions", [&] { return store_->list_sessions(user_id, pool_id); });
    }
    control::Result<void> revoke_session(const std::string& user_id,
                                         const std::string& session_id) override {
        return call("revoke_session", [&] { return store_->revoke_session(user_id, session_id); });
    }
    control::Result<std::vector<control::Device>>
    list_devices(const std::string& user_id) override {
        return call("list_devices", [&] { return store_->list_devices(user_id); });
    }
    control::Result<void> revoke_device(const std::string& user_id,
                                        const std::string& device_id) override {
        return call("revoke_device", [&] { return store_->revoke_device(user_id, device_id); });
    }
    control::Result<control::Principal> context_for(const std::string& secret) override {
        return call("context_for", [&] { return store_->context_for(secret); });
    }
    control::Result<control::OfflineGrant>
    issue_offline_grant(const control::NewOfflineGrant& value) override {
        return call("issue_offline_grant", [&] { return store_->issue_offline_grant(value); });
    }
    control::Result<std::vector<control::OfflineGrant>>
    list_offline_grants(const std::string& user_id) override {
        return call("list_offline_grants", [&] { return store_->list_offline_grants(user_id); });
    }
    control::Result<control::OfflineGrant> get_offline_grant(const std::string& id) override {
        return call("get_offline_grant", [&] { return store_->get_offline_grant(id); });
    }
    control::Result<control::OfflineGrant> revoke_offline_grant(const std::string& user_id,
                                                                const std::string& id) override {
        return call("revoke_offline_grant",
                    [&] { return store_->revoke_offline_grant(user_id, id); });
    }
    control::Result<control::IssuedSession>
    recover_with_code(const std::string& pool_id,
                      const std::string& email,
                      const std::string& code,
                      const std::string& device_name) override {
        return call("recover_with_code",
                    [&] { return store_->recover_with_code(pool_id, email, code, device_name); });
    }
    control::Result<control::RecoveryAttempt>
    begin_email_recovery(const std::string& pool_id,
                         const std::string& email,
                         int cooling_seconds,
                         const std::string& client_address) override {
        return call("begin_email_recovery", [&] {
            return store_->begin_email_recovery(pool_id, email, cooling_seconds, client_address);
        });
    }
    control::Result<control::RecoveryAttempt>
    cancel_recovery(const std::string& user_id, const std::string& recovery_id) override {
        return call("cancel_recovery",
                    [&] { return store_->cancel_recovery(user_id, recovery_id); });
    }
    control::Result<control::RecoveryAttempt>
    begin_admin_recovery(const std::string& organization_id,
                         const std::string& user_id,
                         const std::string& requested_by) override {
        return call("begin_admin_recovery", [&] {
            return store_->begin_admin_recovery(organization_id, user_id, requested_by);
        });
    }
    control::Result<control::RecoveryAttempt>
    get_recovery_attempt(const std::string& recovery_id) override {
        return call("get_recovery_attempt",
                    [&] { return store_->get_recovery_attempt(recovery_id); });
    }
    control::Result<control::RecoveryAttempt>
    approve_admin_recovery(const std::string& recovery_id,
                           const std::string& approved_by) override {
        return call("approve_admin_recovery",
                    [&] { return store_->approve_admin_recovery(recovery_id, approved_by); });
    }
    control::Result<control::IssuedSession>
    complete_recovery(const std::string& recovery_id,
                      const std::string& token,
                      const std::string& device_name) override {
        return call("complete_recovery",
                    [&] { return store_->complete_recovery(recovery_id, token, device_name); });
    }
    control::Result<std::vector<control::RecoveryDelivery>>
    claim_recovery_deliveries(int limit) override {
        return call("claim_recovery_deliveries",
                    [&] { return store_->claim_recovery_deliveries(limit); });
    }
    control::Result<std::vector<control::RecoveryNotification>>
    list_recovery_notifications(const std::string& user_id) override {
        return call("list_recovery_notifications",
                    [&] { return store_->list_recovery_notifications(user_id); });
    }
    control::Result<control::RecoveryContact>
    configure_recovery_contact(const control::RecoveryContact& contact) override {
        return call("configure_recovery_contact",
                    [&] { return store_->configure_recovery_contact(contact); });
    }
    control::Result<control::RecoveryContact>
    get_recovery_contact(const std::string& repository_id) override {
        return call("get_recovery_contact",
                    [&] { return store_->get_recovery_contact(repository_id); });
    }
    control::Result<control::SsoProvider>
    register_sso_provider(const control::NewSsoProvider& value) override {
        return call("register_sso_provider", [&] { return store_->register_sso_provider(value); });
    }
    control::Result<std::vector<control::SsoProvider>>
    list_sso_providers(const std::string& organization_id, int limit) override {
        return call("list_sso_providers",
                    [&] { return store_->list_sso_providers(organization_id, limit); });
    }
    control::Result<control::SsoProvider> get_sso_provider(const std::string& id) override {
        return call("get_sso_provider", [&] { return store_->get_sso_provider(id); });
    }
    control::Result<void> begin_saml(const control::SamlRequest& value) override {
        return call("begin_saml", [&] { return store_->begin_saml(value); });
    }
    control::Result<control::SamlRequest> consume_saml(const control::SamlAnswer& value) override {
        return call("consume_saml", [&] { return store_->consume_saml(value); });
    }
    control::Result<control::OAuthProvider> get_sso_oidc_provider(const std::string& id) override {
        return call("get_sso_oidc_provider", [&] { return store_->get_sso_oidc_provider(id); });
    }
    control::Result<control::SsoProvider> authenticate_scim(const std::string& token) override {
        return call("authenticate_scim", [&] { return store_->authenticate_scim(token); });
    }
    control::Result<control::SsoProvider> test_sso_provider(const std::string& id) override {
        return call("test_sso_provider", [&] { return store_->test_sso_provider(id); });
    }
    control::Result<control::FederationIssuer>
    create_federation_issuer(const control::NewFederationIssuer& value) override {
        return call("create_federation_issuer",
                    [&] { return store_->create_federation_issuer(value); });
    }
    control::Result<std::vector<control::FederationIssuer>>
    list_federation_issuers(const std::string& organization_id, int limit) override {
        return call("list_federation_issuers",
                    [&] { return store_->list_federation_issuers(organization_id, limit); });
    }
    control::Result<bool> allows_federation_claim(const std::string& organization_id,
                                                  const std::string& issuer,
                                                  const std::string& claim) override {
        return call("allows_federation_claim", [&] {
            return store_->allows_federation_claim(organization_id, issuer, claim);
        });
    }
    control::Result<control::IssuedSession>
    sign_in_sso(const control::SsoIdentity& value) override {
        return call("sign_in_sso", [&] { return store_->sign_in_sso(value); });
    }
    control::Result<std::vector<control::ScimUser>> list_scim_users(const std::string& provider_id,
                                                                    int limit) override {
        return call("list_scim_users", [&] { return store_->list_scim_users(provider_id, limit); });
    }
    control::Result<control::ScimUser> get_scim_user(const std::string& provider_id,
                                                     const std::string& resource_id) override {
        return call("get_scim_user",
                    [&] { return store_->get_scim_user(provider_id, resource_id); });
    }
    control::Result<control::ScimUser>
    upsert_scim_user(const control::NewScimUser& value) override {
        return call("upsert_scim_user", [&] { return store_->upsert_scim_user(value); });
    }
    control::Result<control::ScimUser> delete_scim_user(const std::string& provider_id,
                                                        const std::string& resource_id) override {
        return call("delete_scim_user",
                    [&] { return store_->delete_scim_user(provider_id, resource_id); });
    }
    control::Result<std::vector<control::ScimGroup>>
    list_scim_groups(const std::string& provider_id, int limit) override {
        return call("list_scim_groups",
                    [&] { return store_->list_scim_groups(provider_id, limit); });
    }
    control::Result<control::ScimGroup> get_scim_group(const std::string& provider_id,
                                                       const std::string& resource_id) override {
        return call("get_scim_group",
                    [&] { return store_->get_scim_group(provider_id, resource_id); });
    }
    control::Result<control::ScimGroup>
    upsert_scim_group(const control::NewScimGroup& value) override {
        return call("upsert_scim_group", [&] { return store_->upsert_scim_group(value); });
    }
    control::Result<void> delete_scim_group(const std::string& provider_id,
                                            const std::string& resource_id) override {
        return call("delete_scim_group",
                    [&] { return store_->delete_scim_group(provider_id, resource_id); });
    }
    control::Result<std::vector<control::ProvisioningLog>>
    list_sso_provisioning_logs(const std::string& provider_id, int limit) override {
        return call("list_sso_provisioning_logs",
                    [&] { return store_->list_sso_provisioning_logs(provider_id, limit); });
    }

private:
    std::shared_ptr<control::Store> store_;
    Metrics& metrics_;
    std::string kind_;
};

} // namespace

std::shared_ptr<control::Store> instrument_store(std::shared_ptr<control::Store> store,
                                                 Metrics& metrics) {
    if (!store)
        return {};
    return std::make_shared<InstrumentedStore>(std::move(store), metrics);
}

} // namespace lake::telemetry
