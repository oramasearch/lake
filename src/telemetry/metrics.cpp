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
    auto operator<=>(const SeriesKey &) const = default;
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

std::string labels_text(const Labels &labels, std::string_view extra_name = {},
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
    for (const auto &[name, value] : labels)
        append(name, value);
    if (!extra_name.empty())
        append(extra_name, extra_value);
    out += '}';
    return out;
}

template <class Result> std::string result_label(const Result &result) {
    return result ? "ok" : result.error().code;
}

} // namespace

struct Metrics::Impl {
    mutable std::mutex mutex;
    std::map<SeriesKey, double> counters;
    std::map<SeriesKey, double> gauges;
    std::map<SeriesKey, Histogram> histograms;
};

Metrics::Metrics() : impl_(std::make_unique<Impl>()) {
}
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
    Histogram &histogram = impl_->histograms[SeriesKey{std::move(name), std::move(labels)}];
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

    auto scalar_families = [&](const auto &samples, std::string_view type) {
        std::string current_name;
        for (const auto &[key, value] : samples) {
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
    for (const auto &[key, histogram] : impl_->histograms) {
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
    template <class Function> auto call(std::string_view operation, Function &&function) {
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
    InstrumentedStore(std::shared_ptr<control::Store> store, Metrics &metrics)
        : store_(std::move(store)), metrics_(metrics), kind_(store_->kind()) {
    }

    std::string kind() const override {
        return kind_;
    }

    control::Result<void> ping() override {
        return call("ping", [&] {
            return store_->ping();
        });
    }

    control::Result<control::User> create_user(const control::NewUser &value) override {
        return call("create_user", [&] {
            return store_->create_user(value);
        });
    }
    control::Result<std::vector<control::User>> list_users(int limit) override {
        return call("list_users", [&] {
            return store_->list_users(limit);
        });
    }
    control::Result<control::User> get_user(const std::string &id) override {
        return call("get_user", [&] {
            return store_->get_user(id);
        });
    }
    control::Result<control::Organization>
    create_organization(const control::NewOrganization &value) override {
        return call("create_organization", [&] {
            return store_->create_organization(value);
        });
    }
    control::Result<std::vector<control::Organization>> list_organizations(int limit) override {
        return call("list_organizations", [&] {
            return store_->list_organizations(limit);
        });
    }
    control::Result<control::Organization> get_organization(const std::string &id) override {
        return call("get_organization", [&] {
            return store_->get_organization(id);
        });
    }
    control::Result<control::Customer> create_customer(const control::NewCustomer &value) override {
        return call("create_customer", [&] {
            return store_->create_customer(value);
        });
    }
    control::Result<std::vector<control::Customer>> list_customers(int limit) override {
        return call("list_customers", [&] {
            return store_->list_customers(limit);
        });
    }
    control::Result<control::Customer> get_customer(const std::string &id) override {
        return call("get_customer", [&] {
            return store_->get_customer(id);
        });
    }
    control::Result<control::Repository>
    create_repository(const control::NewRepository &value) override {
        return call("create_repository", [&] {
            return store_->create_repository(value);
        });
    }
    control::Result<std::vector<control::Repository>>
    list_repositories(const std::string &customer_id, int limit) override {
        return call("list_repositories", [&] {
            return store_->list_repositories(customer_id, limit);
        });
    }
    control::Result<control::Repository> get_repository(const std::string &id) override {
        return call("get_repository", [&] {
            return store_->get_repository(id);
        });
    }
    control::Result<control::Team> create_team(const control::NewTeam &value) override {
        return call("create_team", [&] {
            return store_->create_team(value);
        });
    }
    control::Result<std::vector<control::Team>> list_teams(const std::string &organization_id,
                                                           int limit) override {
        return call("list_teams", [&] {
            return store_->list_teams(organization_id, limit);
        });
    }
    control::Result<control::Team> get_team(const std::string &id) override {
        return call("get_team", [&] {
            return store_->get_team(id);
        });
    }
    control::Result<control::Membership>
    create_membership(const control::NewMembership &value) override {
        return call("create_membership", [&] {
            return store_->create_membership(value);
        });
    }
    control::Result<std::vector<control::Membership>>
    list_memberships(const std::string &organization_id, const std::string &team_id,
                     int limit) override {
        return call("list_memberships", [&] {
            return store_->list_memberships(organization_id, team_id, limit);
        });
    }
    control::Result<control::Membership>
    accept_invitation(const control::AcceptInvitation &value) override {
        return call("accept_invitation", [&] {
            return store_->accept_invitation(value);
        });
    }
    control::Result<control::Invitation>
    create_invitation(const control::NewInvitation &value) override {
        return call("create_invitation", [&] {
            return store_->create_invitation(value);
        });
    }
    control::Result<std::vector<control::Invitation>>
    list_invitations(const std::string &organization_id, const std::string &team_id,
                     int limit) override {
        return call("list_invitations", [&] {
            return store_->list_invitations(organization_id, team_id, limit);
        });
    }
    control::Result<control::Invitation> get_invitation(const std::string &id) override {
        return call("get_invitation", [&] {
            return store_->get_invitation(id);
        });
    }
    control::Result<control::PolicyObject>
    create_policy_object(const control::NewPolicyObject &value) override {
        return call("create_policy_object", [&] {
            return store_->create_policy_object(value);
        });
    }
    control::Result<control::PolicyObject> get_policy_object(const std::string &id) override {
        return call("get_policy_object", [&] {
            return store_->get_policy_object(id);
        });
    }
    control::Result<control::PolicyObject>
    find_policy_object(const std::string &object_type, const std::string &external_id) override {
        return call("find_policy_object", [&] {
            return store_->find_policy_object(object_type, external_id);
        });
    }
    control::Result<std::vector<control::PolicyObject>>
    list_policy_objects(const std::string &parent_id, const std::string &object_type,
                        int limit) override {
        return call("list_policy_objects", [&] {
            return store_->list_policy_objects(parent_id, object_type, limit);
        });
    }
    control::Result<std::vector<control::ObjectPathEntry>>
    policy_paths(const std::vector<std::string> &requested_object_ids) override {
        return call("policy_paths", [&] {
            return store_->policy_paths(requested_object_ids);
        });
    }
    control::Result<control::Grant> create_grant(const control::NewGrant &value) override {
        return call("create_grant", [&] {
            return store_->create_grant(value);
        });
    }
    control::Result<std::vector<control::Grant>>
    list_grants(const std::vector<std::string> &object_ids,
                const std::optional<std::string> &principal_taap_id, int limit) override {
        return call("list_grants", [&] {
            return store_->list_grants(object_ids, principal_taap_id, limit);
        });
    }

  private:
    std::shared_ptr<control::Store> store_;
    Metrics &metrics_;
    std::string kind_;
};

} // namespace

std::shared_ptr<control::Store> instrument_store(std::shared_ptr<control::Store> store,
                                                 Metrics &metrics) {
    if (!store)
        return {};
    return std::make_shared<InstrumentedStore>(std::move(store), metrics);
}

} // namespace lake::telemetry
