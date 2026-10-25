#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

void register_application_capabilities(Registry& registry);

// The application a routing read found, with its settings (name, allowed origins, redirect URIs,
// theme) read from the record in its workspace Zetta (lake#351). The store's routing error is
// returned as it is; an application without a workspace is `workspace_not_ready`, and one whose
// settings are not in its workspace (registered before migration 0059 and not yet copied) is
// `not_found`. Nothing falls back to the control store. One get_record.
liquid::runtime::task<std::expected<control::Application, ApiResult>>
configured_application(control::Result<control::Application> routing, ApiContext& context);

// The answer of a completed configured_application.
std::expected<control::Application, ApiResult>
configured(std::expected<std::expected<control::Application, ApiResult>, liquid::error> completed);

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

// The refusal of a Lake-pool password entry (sign_up_password, sign_in_password, verify_mfa,
// recover_with_code) on a Lake that signs people in through Clerk (lake#1098), or nullopt when the
// entry may run: in an application's pool, or without Clerk. The rule is
// lake_pool_password_sign_in, the one `viewer` reports as `password_sign_up`.
std::optional<ApiResult> lake_pool_password_refusal(const ApiContext& context,
                                                    std::string_view pool_id);

liquid::runtime::task<ApiResult> emit_application_event(control::Application application,
                                                        std::string event,
                                                        nlohmann::json data,
                                                        ApiContext& context);

liquid::runtime::task<ApiResult> emit_application_mfa_change(ApiContext& context,
                                                             std::string pool_id,
                                                             std::string method,
                                                             bool enabled);

} // namespace lake::api
