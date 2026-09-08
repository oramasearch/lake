#pragma once

#include "lake/control/store.hpp"

namespace lake::test {

inline control::Result<control::Customer> personal_customer(control::Store& store,
                                                            const std::string& user_id) {
    const auto customers = store.list_customers(10000);
    if (!customers) {
        return std::unexpected(customers.error());
    }
    for (const auto& customer : *customers) {
        if (customer.user_id == user_id) {
            return customer;
        }
    }
    return std::unexpected(
        control::StoreError{"not_found", "sign-up did not provision a customer"});
}

} // namespace lake::test
