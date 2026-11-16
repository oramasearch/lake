// SPDX-License-Identifier: MIT

#pragma once

// Clerk webhooks (lake#765, docs/api/clerk.md#webhooks): Clerk's user events create, update and
// disable Lake accounts, so an account exists from the moment Clerk has the user. Deliveries come
// through Svix and are verified with Lake's own HMAC; no SDK.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

namespace lake::api::clerk_webhooks {

inline constexpr std::int64_t tolerance_seconds = 300;

// One delivery: Svix's three headers and the raw body.
struct Delivery {
    std::string id;        // svix-id
    std::string timestamp; // svix-timestamp, Unix seconds
    std::string signature; // svix-signature, space-separated "v1,<base64>"
    std::string body;
};

// The `v1` signature Svix sends: base64 HMAC-SHA256 of "<id>.<timestamp>.<body>" keyed with the
// base64 secret after `whsec_`.
std::string sign(std::string_view secret, const Delivery& delivery);

// Accepts a delivery whose timestamp is within five minutes of `now_unix` and one of whose `v1`
// signatures matches. The reason is returned on refusal.
std::expected<void, std::string>
verify(std::string_view secret, const Delivery& delivery, std::int64_t now_unix = 0);

struct Outcome {
    int status = 200;
    nlohmann::json body;
};

// Verifies, applies once, and audits one delivery against `context.store`:
// user.created provisions the account as a first Clerk sign-in does (managed key, personal
// customer, the subject linked), user.updated changes its email and name, user.deleted disables
// it and revokes its credentials. Other events are acknowledged and ignored.
Outcome receive(ApiContext& context,
                std::string_view secret,
                const Delivery& delivery,
                std::int64_t now_unix = 0);

} // namespace lake::api::clerk_webhooks
