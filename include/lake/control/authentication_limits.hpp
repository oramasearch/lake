#pragma once
#include <chrono>
#include <expected>
#include <map>
#include <mutex>
#include <string>

#include "lake/control/store.hpp"

namespace lake::control {

// The in-memory implementation of the shared reservation policy. Live keys never get evicted.
class AuthenticationAttempts {
public:
    using Clock = std::chrono::steady_clock;
    enum class Refusal { exhausted, capacity };
    explicit AuthenticationAttempts(std::size_t capacity = 100000) : capacity_(capacity) {}

    std::expected<std::string, Refusal> reserve(const std::string& key,
                                                unsigned limit,
                                                const std::string& new_window,
                                                Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        std::erase_if(buckets_, [&](const auto& item) { return item.second.until <= now; });
        auto found = buckets_.find(key);
        if (found == buckets_.end()) {
            if (buckets_.size() >= capacity_)
                return std::unexpected(Refusal::capacity);
            found = buckets_.emplace(key, Bucket{now + std::chrono::minutes(15), new_window, 0, 0})
                        .first;
        }
        if (found->second.failures + found->second.pending >= limit)
            return std::unexpected(Refusal::exhausted);
        ++found->second.pending;
        return found->second.window;
    }

    void finish(const std::string& key,
                const std::string& window,
                AuthenticationOutcome outcome,
                bool reset,
                Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        auto found = buckets_.find(key);
        if (found == buckets_.end() || found->second.window != window || found->second.pending == 0)
            return;
        auto& budget = found->second;
        --budget.pending;
        if (outcome == AuthenticationOutcome::failure)
            ++budget.failures;
        else if (reset && outcome == AuthenticationOutcome::success) {
            budget.failures = 0;
            budget.until = now + std::chrono::minutes(15);
        }
    }

private:
    struct Bucket {
        Clock::time_point until;
        std::string window;
        unsigned failures, pending;
    };
    std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::string, Bucket> buckets_;
};
} // namespace lake::control
