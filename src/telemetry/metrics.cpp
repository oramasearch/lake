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
    list_repositories(const std::string& customer_id, int limit) override {
        return call("list_repositories",
                    [&] { return store_->list_repositories(customer_id, limit); });
    }
    control::Result<control::Repository> get_repository(const std::string& id) override {
        return call("get_repository", [&] { return store_->get_repository(id); });
    }
    control::Result<control::Repository> rename_repository(const std::string& id,
                                                           const std::string& name) override {
        return call("rename_repository", [&] { return store_->rename_repository(id, name); });
    }
    control::Result<control::Repository>
    transfer_repository(const std::string& id, const std::string& customer_id) override {
        return call("transfer_repository",
                    [&] { return store_->transfer_repository(id, customer_id); });
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
    control::Result<control::ForgeRecord>
    create_forge_record(const control::NewForgeRecord& value) override {
        return call("create_forge_record", [&] { return store_->create_forge_record(value); });
    }
    control::Result<control::ForgeRecord> get_forge_record(const std::string& id) override {
        return call("get_forge_record", [&] { return store_->get_forge_record(id); });
    }
    control::Result<std::vector<control::ForgeRecord>> list_forge_records(
        const std::string& repository_id, const std::string& kind, int limit) override {
        return call("list_forge_records",
                    [&] { return store_->list_forge_records(repository_id, kind, limit); });
    }
    control::Result<control::ForgeRecord>
    update_forge_record(const std::string& id, const nlohmann::json& value) override {
        return call("update_forge_record", [&] { return store_->update_forge_record(id, value); });
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
    control::Result<control::Application>
    update_application_theme(const std::string& id, const nlohmann::json& theme) override {
        return call("update_application_theme",
                    [&] { return store_->update_application_theme(id, theme); });
    }
    control::Result<control::ApplicationSigningKey>
    rotate_application_signing_key(const control::ApplicationSigningKey& value) override {
        return call("rotate_application_signing_key",
                    [&] { return store_->rotate_application_signing_key(value); });
    }
    control::Result<control::ApplicationSigningKey>
    current_application_signing_key(const std::string& application_id) override {
        return call("current_application_signing_key",
                    [&] { return store_->current_application_signing_key(application_id); });
    }
    control::Result<std::vector<control::ApplicationSigningKey>>
    list_application_signing_keys(const std::string& application_id) override {
        return call("list_application_signing_keys",
                    [&] { return store_->list_application_signing_keys(application_id); });
    }
    control::Result<control::ApplicationUser>
    register_application_user(const std::string& application_id,
                              const std::string& user_id) override {
        return call("register_application_user",
                    [&] { return store_->register_application_user(application_id, user_id); });
    }
    control::Result<control::ApplicationUser>
    get_application_user(const std::string& application_id, const std::string& user_id) override {
        return call("get_application_user",
                    [&] { return store_->get_application_user(application_id, user_id); });
    }
    control::Result<std::vector<control::ApplicationUser>> list_application_users(
        const std::string& application_id, const std::string& query, int limit) override {
        return call("list_application_users",
                    [&] { return store_->list_application_users(application_id, query, limit); });
    }
    control::Result<control::ApplicationUser>
    set_application_user_status(const std::string& application_id,
                                const std::string& user_id,
                                const std::string& status) override {
        return call("set_application_user_status", [&] {
            return store_->set_application_user_status(application_id, user_id, status);
        });
    }
    control::Result<void> delete_application_user(const std::string& application_id,
                                                  const std::string& user_id) override {
        return call("delete_application_user",
                    [&] { return store_->delete_application_user(application_id, user_id); });
    }
    control::Result<control::ApplicationOrganization>
    create_application_organization(const control::NewApplicationOrganization& value) override {
        return call("create_application_organization",
                    [&] { return store_->create_application_organization(value); });
    }
    control::Result<std::vector<control::ApplicationOrganization>>
    list_application_organizations(const std::string& application_id, int limit) override {
        return call("list_application_organizations",
                    [&] { return store_->list_application_organizations(application_id, limit); });
    }
    control::Result<control::ApplicationOrganization> update_application_organization(
        const std::string& id, const std::string& name, const nlohmann::json& metadata) override {
        return call("update_application_organization",
                    [&] { return store_->update_application_organization(id, name, metadata); });
    }
    control::Result<void> delete_application_organization(const std::string& id) override {
        return call("delete_application_organization",
                    [&] { return store_->delete_application_organization(id); });
    }
    control::Result<control::ApplicationWebhook>
    create_application_webhook(const control::NewApplicationWebhook& value) override {
        return call("create_application_webhook",
                    [&] { return store_->create_application_webhook(value); });
    }
    control::Result<control::ApplicationWebhook>
    get_application_webhook(const std::string& id) override {
        return call("get_application_webhook", [&] { return store_->get_application_webhook(id); });
    }
    control::Result<std::vector<control::ApplicationWebhook>>
    list_application_webhooks(const std::string& application_id, int limit) override {
        return call("list_application_webhooks",
                    [&] { return store_->list_application_webhooks(application_id, limit); });
    }
    control::Result<control::ApplicationWebhookEvent>
    create_application_webhook_event(const std::string& application_id,
                                     const std::string& event,
                                     const nlohmann::json& payload) override {
        return call("create_application_webhook_event", [&] {
            return store_->create_application_webhook_event(application_id, event, payload);
        });
    }
    control::Result<control::ApplicationWebhookEvent>
    get_application_webhook_event(const std::string& id) override {
        return call("get_application_webhook_event",
                    [&] { return store_->get_application_webhook_event(id); });
    }
    control::Result<std::vector<control::ApplicationWebhookEvent>>
    list_application_webhook_events(const std::string& application_id, int limit) override {
        return call("list_application_webhook_events",
                    [&] { return store_->list_application_webhook_events(application_id, limit); });
    }
    control::Result<control::ApplicationWebhookDelivery> create_application_webhook_delivery(
        const control::NewApplicationWebhookDelivery& value) override {
        return call("create_application_webhook_delivery",
                    [&] { return store_->create_application_webhook_delivery(value); });
    }
    control::Result<std::vector<control::ApplicationWebhookDelivery>>
    list_application_webhook_deliveries(const std::string& event_id) override {
        return call("list_application_webhook_deliveries",
                    [&] { return store_->list_application_webhook_deliveries(event_id); });
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
    control::Result<control::DriveShareLink>
    create_drive_share_link(const control::NewDriveShareLink& value) override {
        return call("create_drive_share_link",
                    [&] { return store_->create_drive_share_link(value); });
    }
    control::Result<control::DriveShareLink> get_drive_share_link(const std::string& id) override {
        return call("get_drive_share_link", [&] { return store_->get_drive_share_link(id); });
    }
    control::Result<control::DriveShareLink>
    get_drive_share_link_by_token_hash(const std::string& token_hash) override {
        return call("get_drive_share_link_by_token_hash",
                    [&] { return store_->get_drive_share_link_by_token_hash(token_hash); });
    }
    control::Result<std::vector<control::DriveShareLink>> list_drive_share_links(
        const std::string& repository_id, const std::string& object_id, int limit) override {
        return call("list_drive_share_links", [&] {
            return store_->list_drive_share_links(repository_id, object_id, limit);
        });
    }
    control::Result<control::DriveShareLink>
    revoke_drive_share_link(const std::string& id) override {
        return call("revoke_drive_share_link", [&] { return store_->revoke_drive_share_link(id); });
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
    control::Result<control::MeteringUsage>
    upsert_metering_usage(const control::MeteringUpdate& update) override {
        return call("upsert_metering_usage", [&] { return store_->upsert_metering_usage(update); });
    }
    control::Result<bool> record_metered_build(const control::MeteredBuild& build) override {
        return call("record_metered_build", [&] { return store_->record_metered_build(build); });
    }
    control::Result<std::vector<control::MeteringUsage>>
    list_metering_usage(const std::string& customer_id,
                        const std::string& from_hour,
                        const std::string& to_hour,
                        int limit) override {
        return call("list_metering_usage", [&] {
            return store_->list_metering_usage(customer_id, from_hour, to_hour, limit);
        });
    }
    control::Result<std::optional<std::uint64_t>>
    storage_quota(const std::string& customer_id) override {
        return call("storage_quota", [&] { return store_->storage_quota(customer_id); });
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
    control::Result<std::vector<control::ManagedStore>>
    list_stores(const std::string& customer_id) override {
        return call("list_stores", [&] { return store_->list_stores(customer_id); });
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
    control::Result<std::vector<control::AgentLaunch>> list_agent_launches(
        const std::string& customer_id, const std::string& agent_id, int limit) override {
        return call("list_agent_launches",
                    [&] { return store_->list_agent_launches(customer_id, agent_id, limit); });
    }
    control::Result<control::AgentLaunch> withdraw_agent_launch(const std::string& id) override {
        return call("withdraw_agent_launch", [&] { return store_->withdraw_agent_launch(id); });
    }
    control::Result<control::AgentTask> create_agent_task(const control::AgentTask& value) override {
        return call("create_agent_task", [&] { return store_->create_agent_task(value); });
    }
    control::Result<control::AgentTask> get_agent_task(const std::string& id) override {
        return call("get_agent_task", [&] { return store_->get_agent_task(id); });
    }
    control::Result<control::AgentTask> claim_agent_task(const std::string& id,
                                                         const std::string& agent,
                                                         const std::string& sha256,
                                                         std::uint64_t events_from) override {
        return call("claim_agent_task",
                    [&] { return store_->claim_agent_task(id, agent, sha256, events_from); });
    }
    control::Result<control::AgentTask> record_agent_task_event(const std::string& id,
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
    control::Result<void>
    record_opportunities(const std::vector<control::Opportunity>& rows) override {
        return call("record_opportunities", [&] { return store_->record_opportunities(rows); });
    }
    control::Result<std::vector<control::Opportunity>>
    list_opportunities(const std::string& customer_id) override {
        return call("list_opportunities", [&] { return store_->list_opportunities(customer_id); });
    }
    control::Result<control::Opportunity> set_opportunity_status(const std::string& id,
                                                                 const std::string& status,
                                                                 const std::string& project) override {
        return call("set_opportunity_status",
                    [&] { return store_->set_opportunity_status(id, status, project); });
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
    list_agent_cron_schedules(const std::string& customer_id,
                              const std::string& agent_id) override {
        return call("list_agent_cron_schedules",
                    [&] { return store_->list_agent_cron_schedules(customer_id, agent_id); });
    }
    control::Result<control::AgentCronSchedule>
    withdraw_agent_cron_schedule(const std::string& id) override {
        return call("withdraw_agent_cron_schedule",
                    [&] { return store_->withdraw_agent_cron_schedule(id); });
    }
    control::Result<std::vector<control::AgentLaunch>>
    list_agent_cron_runs(const std::string& customer_id,
                         const std::string& agent_id,
                         int limit) override {
        return call("list_agent_cron_runs",
                    [&] { return store_->list_agent_cron_runs(customer_id, agent_id, limit); });
    }
    control::Result<control::AskMessage>
    append_ask_message(const control::AskMessage& value) override {
        return call("append_ask_message", [&] { return store_->append_ask_message(value); });
    }
    control::Result<std::vector<control::AskMessage>> ask_conversation(
        const std::string& conversation_id, const std::string& user_id, int limit) override {
        return call("ask_conversation",
                    [&] { return store_->ask_conversation(conversation_id, user_id, limit); });
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
    control::Result<control::SecretMount>
    create_secret_mount(const control::NewSecretMount& value,
                        const std::string& instance_kek) override {
        return call("create_secret_mount",
                    [&] { return store_->create_secret_mount(value, instance_kek); });
    }
    control::Result<std::vector<control::SecretMount>> list_secret_mounts(
        const std::string& customer_id, const std::string& organization_id, int limit) override {
        return call("list_secret_mounts", [&] {
            return store_->list_secret_mounts(customer_id, organization_id, limit);
        });
    }
    control::Result<control::SecretMount> get_secret_mount(const std::string& id) override {
        return call("get_secret_mount", [&] { return store_->get_secret_mount(id); });
    }
    control::Result<control::SecretVersion> put_secret(const control::PutSecret& value,
                                                       const std::string& instance_kek) override {
        return call("put_secret", [&] { return store_->put_secret(value, instance_kek); });
    }
    control::Result<control::SecretVersion> read_secret(const std::string& mount_id,
                                                        const std::string& name,
                                                        std::uint64_t version,
                                                        const std::string& instance_kek) override {
        return call("read_secret",
                    [&] { return store_->read_secret(mount_id, name, version, instance_kek); });
    }
    control::Result<control::SecretMetadata> secret_metadata(const std::string& mount_id,
                                                             const std::string& name) override {
        return call("secret_metadata", [&] { return store_->secret_metadata(mount_id, name); });
    }
    control::Result<control::SecretMetadata>
    change_secret_versions(const std::string& mount_id,
                           const std::string& name,
                           const std::vector<std::uint64_t>& versions,
                           control::SecretVersionAction action) override {
        return call("change_secret_versions", [&] {
            return store_->change_secret_versions(mount_id, name, versions, action);
        });
    }
    control::Result<control::IssuedDynamicSecret>
    lease_dynamic_secret(const control::NewDynamicSecretLease& value,
                         const std::string& instance_kek) override {
        return call("lease_dynamic_secret",
                    [&] { return store_->lease_dynamic_secret(value, instance_kek); });
    }
    control::Result<control::DynamicSecretLease>
    get_dynamic_secret_lease(const std::string& id) override {
        return call("get_dynamic_secret_lease",
                    [&] { return store_->get_dynamic_secret_lease(id); });
    }
    control::Result<control::DynamicSecretLease>
    renew_dynamic_secret_lease(const std::string& id, int lifetime_seconds) override {
        return call("renew_dynamic_secret_lease",
                    [&] { return store_->renew_dynamic_secret_lease(id, lifetime_seconds); });
    }
    control::Result<control::DynamicSecretLease>
    revoke_dynamic_secret_lease(const std::string& id) override {
        return call("revoke_dynamic_secret_lease",
                    [&] { return store_->revoke_dynamic_secret_lease(id); });
    }
    control::Result<control::EncryptionKey>
    create_encryption_key(const control::NewEncryptionKey& value,
                          const std::string& instance_kek) override {
        return call("create_encryption_key",
                    [&] { return store_->create_encryption_key(value, instance_kek); });
    }
    control::Result<std::vector<control::EncryptionKey>>
    list_encryption_keys(const std::string& mount_id, int limit) override {
        return call("list_encryption_keys",
                    [&] { return store_->list_encryption_keys(mount_id, limit); });
    }
    control::Result<control::EncryptionKey> get_encryption_key(const std::string& mount_id,
                                                               const std::string& name) override {
        return call("get_encryption_key",
                    [&] { return store_->get_encryption_key(mount_id, name); });
    }
    control::Result<control::EncryptionKey>
    rotate_encryption_key(const std::string& mount_id,
                          const std::string& name,
                          const std::string& instance_kek) override {
        return call("rotate_encryption_key",
                    [&] { return store_->rotate_encryption_key(mount_id, name, instance_kek); });
    }
    control::Result<control::TransitValue> encrypt(const std::string& mount_id,
                                                   const std::string& name,
                                                   const std::string& plaintext,
                                                   const std::string& instance_kek) override {
        return call("encrypt",
                    [&] { return store_->encrypt(mount_id, name, plaintext, instance_kek); });
    }
    control::Result<control::TransitValue> decrypt(const std::string& mount_id,
                                                   const std::string& name,
                                                   const std::string& ciphertext,
                                                   const std::string& instance_kek) override {
        return call("decrypt",
                    [&] { return store_->decrypt(mount_id, name, ciphertext, instance_kek); });
    }
    control::Result<control::TransitValue> rewrap(const std::string& mount_id,
                                                  const std::string& name,
                                                  const std::string& ciphertext,
                                                  const std::string& instance_kek) override {
        return call("rewrap",
                    [&] { return store_->rewrap(mount_id, name, ciphertext, instance_kek); });
    }
    control::Result<control::TransitValue> sign(const std::string& mount_id,
                                                const std::string& name,
                                                const std::string& message,
                                                std::uint64_t version,
                                                const std::string& instance_kek) override {
        return call("sign",
                    [&] { return store_->sign(mount_id, name, message, version, instance_kek); });
    }
    control::Result<control::TransitVerification> verify(const std::string& mount_id,
                                                         const std::string& name,
                                                         const std::string& message,
                                                         const std::string& signature,
                                                         std::uint64_t version,
                                                         const std::string& instance_kek) override {
        return call("verify", [&] {
            return store_->verify(mount_id, name, message, signature, version, instance_kek);
        });
    }
    control::Result<control::TransitValue> hmac(const std::string& mount_id,
                                                const std::string& name,
                                                const std::string& message,
                                                std::uint64_t version,
                                                const std::string& instance_kek) override {
        return call("hmac",
                    [&] { return store_->hmac(mount_id, name, message, version, instance_kek); });
    }
    control::Result<control::DerivedEncryptionKey>
    derive_encryption_key(const std::string& mount_id,
                          const std::string& parent_name,
                          const std::string& name,
                          const std::string& context,
                          const std::string& instance_kek) override {
        return call("derive_encryption_key", [&] {
            return store_->derive_encryption_key(
                mount_id, parent_name, name, context, instance_kek);
        });
    }
    control::Result<control::CustomerKekRotation>
    rotate_customer_kek(const std::string& customer_id, const std::string& instance_kek) override {
        return call("rotate_customer_kek",
                    [&] { return store_->rotate_customer_kek(customer_id, instance_kek); });
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
