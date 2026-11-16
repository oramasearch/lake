// SPDX-License-Identifier: MIT

#pragma once
#include "lake/control/store.hpp"

namespace lake::control {

inline StoreError invalid_authentication() {
    return {"unauthenticated", "invalid credentials"};
}
inline StoreError authentication_cooldown() {
    return {"rate_limited", "authentication temporarily refused; retry after fifteen minutes"};
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
