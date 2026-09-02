#pragma once
#include <memory>
#include <string>

#include "lake/control/store.hpp"

namespace lake::control {

// The production store over the Postgres the operator provisions (LAKE_CONTROL_DB_URI). Calls
// block on libpq for the duration of one statement; they run inline on the caller's thread.
std::unique_ptr<Store> make_postgres_store(std::string connection_uri);

}  // namespace lake::control
