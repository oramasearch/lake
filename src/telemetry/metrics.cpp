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
    control::Result<std::vector<control::RepositoryRef>>
    list_repository_refs(const std::string& repository_id) override {
        return call("list_repository_refs",
                    [&] { return store_->list_repository_refs(repository_id); });
    }
    control::Result<control::RepositoryRef> set_repository_ref(const std::string& repository_id,
                                                               const std::string& name,
                                                               const std::string& head) override {
        return call("set_repository_ref",
                    [&] { return store_->set_repository_ref(repository_id, name, head); });
    }
    control::Result<void> assign_legacy_forge_records() override {
        return call("assign_legacy_forge_records",
                    [&] { return store_->assign_legacy_forge_records(); });
    }
    control::Result<std::vector<control::LegacyForgeRecord>>
    legacy_forge_records(const std::string& after_id, int limit) override {
        return call("legacy_forge_records",
                    [&] { return store_->legacy_forge_records(after_id, limit); });
    }
    control::Result<void> assign_legacy_platform_environments() override {
        return call("assign_legacy_platform_environments",
                    [&] { return store_->assign_legacy_platform_environments(); });
    }
    control::Result<std::vector<control::LegacyPlatformEnvironment>>
    legacy_platform_environments(const std::string& after_id, int limit) override {
        return call("legacy_platform_environments",
                    [&] { return store_->legacy_platform_environments(after_id, limit); });
    }
    control::Result<void> assign_legacy_platform_mirror() override {
        return call("assign_legacy_platform_mirror",
                    [&] { return store_->assign_legacy_platform_mirror(); });
    }
    control::Result<std::vector<control::LegacyPlatformMirrorRow>>
    legacy_platform_mirror(const std::string& after_id, int limit) override {
        return call("legacy_platform_mirror",
                    [&] { return store_->legacy_platform_mirror(after_id, limit); });
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
    control::Result<void> assign_legacy_application_settings() override {
        return call("assign_legacy_application_settings",
                    [&] { return store_->assign_legacy_application_settings(); });
    }
    control::Result<std::vector<control::LegacyApplicationSettings>>
    legacy_application_settings(const std::string& after_id, int limit) override {
        return call("legacy_application_settings",
                    [&] { return store_->legacy_application_settings(after_id, limit); });
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
    control::Result<void> assign_legacy_application_directory() override {
        return call("assign_legacy_application_directory",
                    [&] { return store_->assign_legacy_application_directory(); });
    }
    control::Result<std::vector<control::LegacyApplicationUser>>
    legacy_application_users(const std::string& after_id, int limit) override {
        return call("legacy_application_users",
                    [&] { return store_->legacy_application_users(after_id, limit); });
    }
    control::Result<std::vector<control::LegacyApplicationOrganization>>
    legacy_application_organizations(const std::string& after_id, int limit) override {
        return call("legacy_application_organizations",
                    [&] { return store_->legacy_application_organizations(after_id, limit); });
    }
    control::Result<void> assign_legacy_application_credentials() override {
        return call("assign_legacy_application_credentials",
                    [&] { return store_->assign_legacy_application_credentials(); });
    }
    control::Result<std::vector<control::LegacyApplicationSigningKeyRing>>
    legacy_application_signing_keys(const std::string& after_application_id, int limit) override {
        return call("legacy_application_signing_keys", [&] {
            return store_->legacy_application_signing_keys(after_application_id, limit);
        });
    }
    control::Result<std::vector<control::LegacyApplicationWebhook>>
    legacy_application_webhooks(const std::string& after_id, int limit) override {
        return call("legacy_application_webhooks",
                    [&] { return store_->legacy_application_webhooks(after_id, limit); });
    }
    control::Result<void> assign_legacy_application_webhook_events() override {
        return call("assign_legacy_application_webhook_events",
                    [&] { return store_->assign_legacy_application_webhook_events(); });
    }
    control::Result<std::vector<control::LegacyApplicationWebhookEvent>>
    legacy_application_webhook_events(const std::string& after_id, int limit) override {
        return call("legacy_application_webhook_events",
                    [&] { return store_->legacy_application_webhook_events(after_id, limit); });
    }
    control::Result<std::vector<control::LegacyApplicationWebhookDelivery>>
    legacy_application_webhook_deliveries(const std::string& after_id, int limit) override {
        return call("legacy_application_webhook_deliveries",
                    [&] { return store_->legacy_application_webhook_deliveries(after_id, limit); });
    }
    control::Result<void> create_application_authorization_code(
        const control::ApplicationAuthorizationCode& value) override {
        return call("create_application_authorization_code",
                    [&] { return store_->create_application_authorization_code(value); });
    }
    control::Result<control::ApplicationAuthorizationCode>
    consume_application_authorization_code(const std::string& application_id,
                                           const std::string& code_hash) override {
        return call("consume_application_authorization_code", [&] {
            return store_->consume_application_authorization_code(application_id, code_hash);
        });
    }
    control::Result<control::DriveShareLinkRoute>
    drive_share_link_route(const std::string& token_hash) override {
        return call("drive_share_link_route",
                    [&] { return store_->drive_share_link_route(token_hash); });
    }
    control::Result<void> assign_legacy_drive_share_links() override {
        return call("assign_legacy_drive_share_links",
                    [&] { return store_->assign_legacy_drive_share_links(); });
    }
    control::Result<std::vector<control::LegacyDriveShareLink>>
    legacy_drive_share_links(const std::string& after_id, int limit) override {
        return call("legacy_drive_share_links",
                    [&] { return store_->legacy_drive_share_links(after_id, limit); });
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
    control::Result<void> assign_legacy_metering() override {
        return call("assign_legacy_metering", [&] { return store_->assign_legacy_metering(); });
    }
    control::Result<std::vector<control::LegacyMeteringUsage>>
    legacy_metering_usage(const std::string& after_id, int limit) override {
        return call("legacy_metering_usage",
                    [&] { return store_->legacy_metering_usage(after_id, limit); });
    }
    control::Result<std::vector<control::LegacyMeteredBuild>>
    legacy_metering_build_records(const std::string& after_id, int limit) override {
        return call("legacy_metering_build_records",
                    [&] { return store_->legacy_metering_build_records(after_id, limit); });
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
    control::Result<void> assign_legacy_stream_owners() override {
        return call("assign_legacy_stream_owners",
                    [&] { return store_->assign_legacy_stream_owners(); });
    }
    control::Result<std::vector<control::LegacyStreamOwner>>
    legacy_stream_owners(const std::string& after_id, int limit) override {
        return call("legacy_stream_owners",
                    [&] { return store_->legacy_stream_owners(after_id, limit); });
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
                            const std::string& phase,
                            const std::string& message,
                            const nlohmann::json& result) override {
        return call("record_agent_task_event", [&] {
            return store_->record_agent_task_event(id, agent, phase, message, result);
        });
    }
    control::Result<void> fail_agent_task(const std::string& id,
                                          const std::string& message) override {
        return call("fail_agent_task", [&] { return store_->fail_agent_task(id, message); });
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
    control::Result<void> assign_legacy_opportunities() override {
        return call("assign_legacy_opportunities",
                    [&] { return store_->assign_legacy_opportunities(); });
    }
    control::Result<std::vector<control::LegacyOpportunity>>
    legacy_opportunities(const std::string& after_id, int limit) override {
        return call("legacy_opportunities",
                    [&] { return store_->legacy_opportunities(after_id, limit); });
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
    control::Result<control::PkiRecoveryCeremonyRecord> complete_pki_recovery_ceremony(
        const std::string& customer_id,
        const std::string& ceremony_id,
        const std::vector<control::PkiRecoveryShareEvidence>& evidence) override {
        return call("complete_pki_recovery_ceremony", [&] {
            return store_->complete_pki_recovery_ceremony(customer_id, ceremony_id, evidence);
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
    control::Result<void> assign_legacy_secrets() override {
        return call("assign_legacy_secrets", [&] { return store_->assign_legacy_secrets(); });
    }
    control::Result<std::vector<control::LegacySecretMount>>
    legacy_secret_mounts(const std::string& after_id, int limit) override {
        return call("legacy_secret_mounts",
                    [&] { return store_->legacy_secret_mounts(after_id, limit); });
    }
    control::Result<std::vector<control::LegacySecretEntry>>
    legacy_secret_entries(const std::string& after_id, int limit) override {
        return call("legacy_secret_entries",
                    [&] { return store_->legacy_secret_entries(after_id, limit); });
    }
    control::Result<std::vector<control::LegacySecretVersion>>
    legacy_secret_versions(const std::string& after_id, int limit) override {
        return call("legacy_secret_versions",
                    [&] { return store_->legacy_secret_versions(after_id, limit); });
    }
    control::Result<std::vector<control::LegacyEncryptionKey>>
    legacy_encryption_keys(const std::string& after_id, int limit) override {
        return call("legacy_encryption_keys",
                    [&] { return store_->legacy_encryption_keys(after_id, limit); });
    }
    control::Result<std::vector<control::LegacySecretDataKey>>
    legacy_secret_data_keys(const std::string& after_id, int limit) override {
        return call("legacy_secret_data_keys",
                    [&] { return store_->legacy_secret_data_keys(after_id, limit); });
    }
    control::Result<std::vector<control::LegacyDynamicSecretLease>>
    legacy_dynamic_secret_leases(const std::string& after_id, int limit) override {
        return call("legacy_dynamic_secret_leases",
                    [&] { return store_->legacy_dynamic_secret_leases(after_id, limit); });
    }

    control::Result<control::IssuedSession>
    sign_up_password(const control::NewPasswordUser& value) override {
        return call("sign_up_password", [&] { return store_->sign_up_password(value); });
    }
    control::Result<control::IssuedSession>
    sign_in_password(const control::PasswordSignIn& value) override {
        return call("sign_in_password", [&] { return store_->sign_in_password(value); });
    }
    control::Result<control::IssuedSession>
    sign_in_taap(const control::TaapSignIn& value) override {
        return call("sign_in_taap", [&] { return store_->sign_in_taap(value); });
    }
    control::Result<control::MfaEnrollment> enroll_mfa(const std::string& user_id,
                                                       const std::string& pool_id) override {
        return call("enroll_mfa", [&] { return store_->enroll_mfa(user_id, pool_id); });
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
                                                      const std::string& state) override {
        return call("consume_oauth", [&] { return store_->consume_oauth(provider, state); });
    }
    control::Result<control::IssuedSession> create_token(const control::NewToken& value) override {
        return call("create_token", [&] { return store_->create_token(value); });
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
    control::Result<control::RecoveryAttempt> begin_email_recovery(const std::string& pool_id,
                                                                   const std::string& email,
                                                                   int cooling_seconds) override {
        return call("begin_email_recovery",
                    [&] { return store_->begin_email_recovery(pool_id, email, cooling_seconds); });
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
