#pragma once

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// Authentication uses at most two of the bounded blocking pool's workers, with no additional
// queue. Hold admission until the callable has finished, including during cancellation/drain.
inline std::atomic<unsigned> authentication_work_count{0};

template <class Callable>
boost::capy::task<std::invoke_result_t<Callable>> authentication_work(Callable callable) {
    unsigned active = authentication_work_count.load();
    do {
        if (active >= 2)
            co_return std::unexpected(
                control::StoreError{"rate_limited", "authentication is busy; retry later"});
    } while (!authentication_work_count.compare_exchange_weak(active, active + 1));
    auto permit =
        std::shared_ptr<void>(nullptr, [](void*) { authentication_work_count.fetch_sub(1); });
    auto ran = co_await run_blocking(
        [callable = std::move(callable), permit = std::move(permit)]() mutable {
            return std::invoke(callable);
        });
    if (!ran)
        co_return std::unexpected(
            control::StoreError{"store_unavailable", "authentication worker unavailable"});
    co_return std::move(*ran);
}

} // namespace lake::api
