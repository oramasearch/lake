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
    {{0.001, "0.001"}, {0.005, "0.005"}, {0.01, "0.01"}, {0.025, "0.025"}, {0.05, "0.05"},
     {0.1, "0.1"}, {0.25, "0.25"}, {0.5, "0.5"}, {1.0, "1"}, {2.5, "2.5"}, {5.0, "5"}},
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
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string labels_text(const Labels& labels, std::string_view extra_name = {}, std::string_view extra_value = {}) {
    if (labels.empty() && extra_name.empty()) return {};
    std::string out = "{";
    bool first = true;
    auto append = [&](std::string_view name, std::string_view value) {
        if (!first) out += ',';
        first = false;
        out += name;
        out += "=\"";
        out += escaped_label(value);
        out += '"';
    };
    for (const auto& [name, value] : labels) append(name, value);
    if (!extra_name.empty()) append(extra_name, extra_value);
    out += '}';
    return out;
}

template <class Result>
std::string result_label(const Result& result) {
    return result ? "ok" : result.error().code;
}

}  // namespace

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
        if (value <= latency_buckets[i].first) ++histogram.buckets[i];
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
            out += key.name + "_bucket" + labels_text(key.labels, "le", latency_buckets[i].second) + " " +
                   std::to_string(histogram.buckets[i]) + "\n";
        }
        out += key.name + "_bucket" + labels_text(key.labels, "le", "+Inf") + " " +
               std::to_string(histogram.count) + "\n";
        out += key.name + "_sum" + labels_text(key.labels) + " " + number(histogram.sum) + "\n";
        out += key.name + "_count" + labels_text(key.labels) + " " + std::to_string(histogram.count) + "\n";
    }
    return out;
}

namespace {

class InstrumentedStore final : public control::Store {
private:
    template <class Function>
    auto call(std::string_view operation, Function&& function) {
        const auto start = std::chrono::steady_clock::now();
        auto result = function();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const Labels labels{{"store", kind_}, {"operation", std::string(operation)}, {"result", result_label(result)}};
        metrics_.increment_counter("lake_store_calls_total", labels);
        metrics_.observe_histogram("lake_store_call_duration_seconds", labels, seconds);
        return result;
    }

public:
    InstrumentedStore(std::shared_ptr<control::Store> store, Metrics& metrics)
        : store_(std::move(store)), metrics_(metrics), kind_(store_->kind()) {}

    std::string kind() const override { return kind_; }

    control::Result<control::User> create_user(const control::NewUser& value) override {
        return call("create_user", [&] { return store_->create_user(value); });
    }
    control::Result<std::vector<control::User>> list_users(int limit) override {
        return call("list_users", [&] { return store_->list_users(limit); });
    }
    control::Result<control::User> get_user(const std::string& id) override {
        return call("get_user", [&] { return store_->get_user(id); });
    }
    control::Result<control::Organization> create_organization(const control::NewOrganization& value) override {
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
    control::Result<std::vector<control::Customer>> list_customers(int limit) override {
        return call("list_customers", [&] { return store_->list_customers(limit); });
    }
    control::Result<control::Customer> get_customer(const std::string& id) override {
        return call("get_customer", [&] { return store_->get_customer(id); });
    }
    control::Result<control::Repository> create_repository(const control::NewRepository& value) override {
        return call("create_repository", [&] { return store_->create_repository(value); });
    }
    control::Result<std::vector<control::Repository>> list_repositories(const std::string& customer_id,
                                                                        int limit) override {
        return call("list_repositories", [&] { return store_->list_repositories(customer_id, limit); });
    }
    control::Result<control::Repository> get_repository(const std::string& id) override {
        return call("get_repository", [&] { return store_->get_repository(id); });
    }

private:
    std::shared_ptr<control::Store> store_;
    Metrics& metrics_;
    std::string kind_;
};

}  // namespace

std::shared_ptr<control::Store> instrument_store(std::shared_ptr<control::Store> store, Metrics& metrics) {
    if (!store) return {};
    return std::make_shared<InstrumentedStore>(std::move(store), metrics);
}

}  // namespace lake::telemetry
