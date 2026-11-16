// SPDX-License-Identifier: MIT

#pragma once

// Resend, the email vendor lake sends its own transactional email through (lake#831,
// docs/api/orders.md#email): lake's own client for Resend's REST API (POST /emails with a bearer
// key and an Idempotency-Key), never Resend's SDK. The HTTP exchange is the server's outbound
// client, injected as a ResendHttp the tests replace with a test double. The key is the
// instance's platform secret (LAKE_RESEND_API_KEY), an environment variable of the hosting platform: never a
// workspace secret, never logged, never in an error message.

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lake::api {

inline constexpr std::string_view resend_api_base = "https://api.resend.com";
// No default sender or support address: an install names its own, and sends no email until it does.
inline constexpr std::string_view default_support_email = "";
inline constexpr std::string_view default_resend_from = "";

struct ResendRequest {
    std::string method = "POST";
    std::string url;
    std::string body; // application/json
    std::vector<std::pair<std::string, std::string>> headers;
};
struct ResendResponse {
    int status = 0;
    std::string body;
    std::string error; // the transport's failure, never the request
};
using ResendHttp = std::function<ResendResponse(const ResendRequest&)>;

// Where email goes from and to: LAKE_RESEND_API_KEY, LAKE_RESEND_FROM (an address on a domain
// verified in Resend, e.g. "Lake <orders@example.com>") and LAKE_SUPPORT_EMAIL (where orders go).
// All three must be set for lake to send email; none has a default.
struct ResendSettings {
    std::string api_key;
    std::string from = std::string(default_resend_from);
    std::string support_email = std::string(default_support_email);
    // Why lake cannot send: empty when it can (the key, the sender and the support address are all
    // set). Names the variable, never a value.
    std::string unavailable_reason() const;
};

// Reads the three variables' values (empty = unset: lake sends no email). An error names the
// variable: a key that is not a Resend key (re_...), a from or support address that is not an
// email address (a "Name <address>" from is accepted).
std::expected<ResendSettings, std::string> resend_settings_from_environment(
    std::string_view api_key, std::string_view from, std::string_view support_email);

// Whether `address` looks like one email address: one @, a local part and a dotted domain, no
// spaces or angle brackets, at most 254 characters. Not a deliverability check.
bool plausible_email_address(std::string_view address);

struct Email {
    std::vector<std::string> to;
    std::string subject;
    std::string text;
    std::string reply_to; // empty: none
};

// Sends one email through Resend: POST <base>/emails with {from, to, subject, text, reply_to?},
// `Authorization: Bearer <key>` and `Idempotency-Key: <idempotency_key>` (Resend answers a repeat
// of the key within 24 hours with the first send instead of sending again). Answers Resend's email
// id, or why it failed: the transport's error, or Resend's status and message (on 401 and 403 only
// that the key was refused, so nothing of the key is echoed).
std::expected<std::string, std::string> send_resend_email(const ResendSettings& settings,
                                                          const Email& email,
                                                          std::string_view idempotency_key,
                                                          const ResendHttp& http,
                                                          std::string_view base = resend_api_base);

} // namespace lake::api
