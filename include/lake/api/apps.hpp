#pragma once

#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

void register_application_capabilities(Registry& registry);

// Completes a session the store issued. In the lake pool it is the session and its secret. In an
// application's pool the session is returned only to an active user of the application, read in
// the application's workspace Zetta (lake#353): a first OAuth sign-in records the user there, and
// any other account without an active record has the session revoked and is refused. The answer
// then carries the application JWT, and the lifecycle events are recorded.
liquid::runtime::task<ApiResult>
application_session_result(control::Result<control::IssuedSession> issued,
                           ApiContext& context,
                           std::string lifecycle_event);

// The same check without the JWT or events, for sessions account recovery issues.
liquid::runtime::task<ApiResult>
checked_application_session(control::Result<control::IssuedSession> issued, ApiContext& context);

// A password sign-up. In an application's pool the email is first claimed in the application's
// workspace, with `if_absent`, for an account id chosen here; lake then creates the account with
// that id, and the session completes as above, writing the user record.
liquid::runtime::task<ApiResult> sign_up_password_session(control::NewPasswordUser user,
                                                          ApiContext& context);

liquid::runtime::task<ApiResult> emit_application_event(control::Application application,
                                                        std::string event,
                                                        nlohmann::json data,
                                                        ApiContext& context);

liquid::runtime::task<ApiResult> emit_application_mfa_change(ApiContext& context,
                                                             std::string pool_id,
                                                             std::string method,
                                                             bool enabled);

} // namespace lake::api
