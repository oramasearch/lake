#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

void register_application_capabilities(Registry& registry);

ApiResult application_session_result(control::Result<control::IssuedSession> issued,
                                     ApiContext& context,
                                     std::string_view lifecycle_event);

ApiResult emit_application_event(const control::Application& application,
                                 std::string_view event,
                                 const nlohmann::json& data,
                                 ApiContext& context);

ApiResult emit_application_mfa_change(ApiContext& context,
                                      std::string_view pool_id,
                                      std::string_view method,
                                      bool enabled);

} // namespace lake::api
