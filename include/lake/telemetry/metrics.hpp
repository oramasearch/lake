#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lake::control {
class Store;
}

namespace lake::telemetry {

using Labels = std::vector<std::pair<std::string, std::string>>;

// A process-local Prometheus registry. Histograms use lake's fixed latency buckets and all
// mutation is safe from the HTTP and store call sites.
class Metrics {
public:
    Metrics();
    ~Metrics();
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void increment_counter(std::string name, Labels labels = {}, double amount = 1.0);
    void set_gauge(std::string name, Labels labels, double value);
    void observe_histogram(std::string name, Labels labels, double value);

    [[nodiscard]] std::string prometheus_text() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Decorate a control store so each operation records its result and duration in metrics.
std::shared_ptr<control::Store> instrument_store(std::shared_ptr<control::Store> store, Metrics& metrics);

}  // namespace lake::telemetry
