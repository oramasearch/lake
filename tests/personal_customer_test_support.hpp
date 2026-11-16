// SPDX-License-Identifier: MIT

#pragma once

#include "lake/control/store.hpp"

namespace lake::test {

inline control::Result<control::Customer> personal_customer(control::Store& store,
                                                            const std::string& user_id) {
    const auto customers = store.list_customers(2, user_id);
    if (!customers) {
        return std::unexpected(customers.error());
    }
    if (customers->size() == 1) {
        return customers->front();
    }
    return std::unexpected(
        control::StoreError{"not_found", "sign-up did not provision exactly one customer"});
}

} // namespace lake::test
