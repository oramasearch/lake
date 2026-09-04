#pragma once
#include "lake/api/registry.hpp"

namespace lake::api {

// The core capability set: server_info (no auth) and the control-plane entities — users,
// organizations, customers — as list / get / create. Handlers are thin callers of the Store.
void register_core_capabilities(Registry& registry, std::string_view version);
void register_secret_capabilities(Registry& registry);

} // namespace lake::api
