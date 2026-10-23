#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace lake::control {

// Fixed windows retain active buckets until expiry. A full table refuses new keys rather than
// evicting an account's budget; arbitrary account/client names cannot reset another bucket.
class AuthenticationAttempts {
public:
    using Clock = std::chrono::steady_clock;
    explicit AuthenticationAttempts(unsigned limit, std::size_t capacity = 4096)
        : limit_(limit), capacity_(capacity) {}

    bool admit(const std::string& key, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex_);
        auto found = buckets_.find(key);
        if (found != buckets_.end() && now < found->second.until) {
            if (found->second.count >= limit_)
                return false;
            ++found->second.count;
            return true;
        }
        if (found == buckets_.end()) {
            if (buckets_.size() >= capacity_) {
                std::erase_if(buckets_,
                              [now](const auto& item) { return now >= item.second.until; });
            }
            if (buckets_.size() >= capacity_)
                return false;
        }
        buckets_[key] = {now + std::chrono::minutes(1), 1};
        return true;
    }

    void refund(const std::string& key, Clock::time_point admitted_at) {
        std::lock_guard lock(mutex_);
        const auto found = buckets_.find(key);
        if (found != buckets_.end() && admitted_at < found->second.until &&
            admitted_at >= found->second.until - std::chrono::minutes(1) && found->second.count > 0)
            --found->second.count;
    }

private:
    struct Bucket {
        Clock::time_point until;
        unsigned count;
    };
    unsigned limit_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::string, Bucket> buckets_;
};

} // namespace lake::control
