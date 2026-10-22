#pragma once
#include <charconv>
#include <cstdlib>

#include "lake/control/store.hpp"

namespace lake::control {

inline StoreError invalid_authentication() {
    return {"unauthenticated", "invalid credentials"};
}
inline StoreError authentication_cooldown() {
    return {"rate_limited", "authentication temporarily refused; retry after fifteen minutes"};
}
inline Result<unsigned> authentication_global_failure_limit() {
    const char* configured = std::getenv("LAKE_AUTH_GLOBAL_FAILURE_LIMIT");
    if (!configured)
        return 1000;
    const std::string_view text(configured);
    unsigned limit = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), limit);
    if (error != std::errc{} || end != text.data() + text.size() || limit == 0 || limit > 1000000)
        return std::unexpected(StoreError{"store_error", "invalid LAKE_AUTH_GLOBAL_FAILURE_LIMIT"});
    return limit;
}

// Reservations include in-flight work so concurrent guesses cannot outrun a failure limit.
// A failed completion write refuses the request; an interrupted request retains its reservation.
template <class Verify>
auto authentication_call(Store& store,
                         const std::string& pool,
                         const std::string& email,
                         const std::string& user,
                         Verify&& verify)
    -> decltype(verify(std::declval<AuthenticationOutcome&>())) {
    auto attempt = store.authentication_attempt(pool, email, user, "");
    if (!attempt)
        return std::unexpected(attempt.error());
    auto outcome = AuthenticationOutcome::failure;
    auto result = verify(outcome);
    if (result)
        outcome = AuthenticationOutcome::success;
    else if (result.error().code != "unauthorized" && result.error().code != "not_found" &&
             result.error().code != "unauthenticated")
        outcome = AuthenticationOutcome::neutral;
    const auto finished = store.authentication_attempt_finished(*attempt, outcome);
    if (!finished)
        return std::unexpected(finished.error());
    if (!result && outcome == AuthenticationOutcome::failure)
        return std::unexpected(invalid_authentication());
    return result;
}
} // namespace lake::control
