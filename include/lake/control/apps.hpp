#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// An application as callers see it. Its routing is lake's content-free auth_applications row:
// `id`, `customer_id`, `pool_id`, `custom_sign_in_domain`, `workspace_id`, `created_at` and
// `updated_at`, which find the application from a request before any workspace is read (a pool's
// sessions, a hosted page's host, an anonymous JWKS read). Its settings, `name`,
// `allowed_origins`, `redirect_uris` and `theme`, are the ApplicationSettings record in the
// workspace Zetta of its workspace (lake#351, control/application_settings_records.hpp); the store
// returns them empty, and the API layer fills them from the workspace.
struct Application {
    std::string id;
    std::string customer_id;
    std::string pool_id;
    std::string name;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> redirect_uris;
    std::string custom_sign_in_domain;
    nlohmann::json theme = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
    // The workspace whose Zetta holds the application's settings, keys, webhooks, users,
    // organizations, lifecycle events and delivery attempts. Set at registration and never
    // changed; empty only for an application registered before migration 0049 whose space had
    // several workspaces, until it is assigned.
    std::string workspace_id;
};

// The settings of an application (lake#351): one record in the workspace Zetta of the
// application's workspace, keyed by the application id. `customer_id` binds it to the
// application's space. `revision` is the engine revision it was read at and `lake_revision` lake's
// own (1 when written, one more for each change).
struct ApplicationSettings {
    std::string application_id;
    std::string customer_id;
    std::string name;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> redirect_uris;
    nlohmann::json theme = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};

// The settings columns of a retired auth_applications row (migration 0059), with its recorded owner,
// for the one-shot copy only: the application's workspace, or empty with the reason it is not
// (`needs_selection`, `workspace_deleted`).
struct LegacyApplicationSettings {
    ApplicationSettings settings;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};

// The application with `settings` filled in from its workspace record.
void apply_settings(Application& application, const ApplicationSettings& settings);

// One Ed25519 key of an application. `seed` is the private key in the clear, present only between
// generating or opening it and signing; what is kept is `encrypted_seed`, the secrets subsystem's
// envelope (purpose `application.jwt.<application id>`), inside the sealed key ring record of the
// application's workspace (lake#352).
struct ApplicationSigningKey {
    std::string application_id;
    std::string kid;
    std::string seed;
    std::string public_key;
    int version = 0;
    bool current = false;
    std::string created_at;
    std::string retired_at;
    std::string encrypted_seed;
};

// Every signing key of one application, in version order with exactly one current: one record in
// the workspace Zetta of the application's workspace, so a rotation is one conditional write.
// `revision` is the engine revision it was read at and `lake_revision` lake's own (1 until the
// first rotation after the record was written).
struct ApplicationSigningKeyRing {
    std::string application_id;
    std::vector<ApplicationSigningKey> keys;
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
    std::int64_t revision = 0;
};

// The routing row of a new application. Its settings are written to its workspace first
// (lake#351), never to the control store.
struct NewApplication {
    std::string id;
    std::string customer_id;
    std::string pool_id;
    std::string custom_sign_in_domain;
    std::string workspace_id; // a live workspace of the customer (migration 0049)
};

// One user of an application. It lives in the workspace Zetta of the application's workspace
// (lake#353, control/application_user_records.hpp), not in lake's PostgreSQL. `own_pool` is true
// for a user who signed up in the application's own pool; that user's email is claimed once per
// application there. A lake user who authorized the application keeps their lake email, unclaimed.
struct ApplicationUser {
    std::string application_id;
    std::string user_id;
    std::string taap_id;
    std::string email;
    std::string display_name;
    std::string status;
    std::string created_at;
    std::string updated_at;
    bool own_pool = false;
    std::int64_t at_us = 0;         // created_at in microseconds since the Unix epoch
    std::int64_t revision = 0;      // the engine revision it was read at
    std::int64_t lake_revision = 0; // 1 when written, one more for each change
};

// One organization an application manages for its users, in the same workspace (lake#354). Its
// TAAP identity and its name are each unique within the application.
struct ApplicationOrganization {
    std::string id;
    std::string application_id;
    std::string taap_id;
    std::string name;
    nlohmann::json metadata = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 0;
};

// Rows of the retired auth_app_users and auth_app_organizations tables with their recorded owner
// (migration 0055), for the one-shot copy only. A user row's `id` is md5(application id, newline,
// user id) as a UUID, since the table has no id column.
struct LegacyApplicationUser {
    std::string id;
    ApplicationUser user;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
struct LegacyApplicationOrganization {
    ApplicationOrganization organization;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};

struct NewApplicationOrganization {
    std::string application_id;
    std::string taap_id;
    std::string name;
    nlohmann::json metadata = nlohmann::json::object();
};

// A lifecycle webhook of an application, kept in the workspace Zetta of the application's
// workspace (lake#355). `secret` is the signing secret in the clear, present only between creating
// or opening it and signing; what is kept is `encrypted_secret`, the secrets subsystem's envelope
// (purpose `application.webhook.<webhook id>`), inside the sealed record.
struct ApplicationWebhook {
    std::string id;
    std::string application_id;
    std::string url;
    std::vector<std::string> events;
    std::string secret;
    bool enabled = true;
    std::string created_at;
    std::string updated_at;
    std::string encrypted_secret;
    std::int64_t at_us = 0; // created_at in microseconds since the Unix epoch
};

// One lifecycle event of an application. It lives in the workspace Zetta of the application's
// workspace (lake#356, control/application_webhook_records.hpp).
struct ApplicationWebhookEvent {
    std::string id;
    std::string application_id;
    std::string event;
    nlohmann::json payload = nlohmann::json::object();
    std::string created_at;
    std::int64_t at_us = 0; // created_at in microseconds since the Unix epoch
};

// One delivery attempt of an event to a webhook, in the same workspace as its event (lake#357).
// `pending` is true from the moment a caller claims the attempt until it records the outcome: a
// claimed attempt is sent by that caller only.
struct ApplicationWebhookDelivery {
    std::string id;
    std::string event_id;
    std::string webhook_id;
    int attempt = 0;
    int status = 0;
    std::string signature;
    std::string error;
    std::string created_at;
    std::string application_id;
    bool pending = false;
    std::int64_t at_us = 0;
};

// Rows of the retired auth_app_webhook_events and auth_app_webhook_deliveries tables with their
// recorded owner (migration 0049), for the one-shot copy only: the workspace each is copied into,
// or empty with the reason it is not (`needs_selection`). A delivery's owner is its event's.
struct LegacyApplicationWebhookEvent {
    ApplicationWebhookEvent event;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
struct LegacyApplicationWebhookDelivery {
    ApplicationWebhookDelivery delivery;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};

// Rows of the retired auth_app_signing_keys and auth_app_webhooks tables with their recorded owner
// (migration 0056), for the one-shot copy only. A key ring is every key row of one application;
// its owner, like each webhook's, is the application's workspace, or empty with the reason it is
// not (`needs_selection`). Credentials stay the envelopes the table held.
struct LegacyApplicationSigningKeyRing {
    ApplicationSigningKeyRing ring;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};
struct LegacyApplicationWebhook {
    ApplicationWebhook webhook;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};

// A one-use OIDC authorization code of an application (lake#363): issued to a signed-in user and
// redeemed once by the relying party's backend within five minutes. It lives in the workspace Zetta
// of the application's workspace, keyed by the blind index of its hash; the code itself is never
// kept. `at_us` is when it was issued and `expires_us` when it stops being redeemable, both in
// microseconds since the Unix epoch; `revision` is the engine revision it was read at.
struct ApplicationAuthorizationCode {
    std::string application_id;
    std::string user_id;
    std::string redirect_uri;
    std::string code_hash;
    std::string code_challenge;
    std::string nonce;
    std::int64_t at_us = 0;
    std::int64_t expires_us = 0;
    std::int64_t revision = 0;
};

// A live (unused, unexpired) row of the retired auth_app_authorization_codes table with its
// recorded owner (migration 0061), for the one-shot copy only: the application's workspace
// (`application_workspace`, or `copied` once the copy has written it), or empty with the reason it
// is not (`needs_selection`, `workspace_deleted`).
struct LegacyApplicationAuthorizationCode {
    ApplicationAuthorizationCode code;
    std::string customer_id;
    std::string workspace_id;
    std::string outcome;
};

namespace apps {

using CryptoResult = std::expected<ApplicationSigningKey, std::string>;

CryptoResult generate_signing_key(std::string_view application_id, int version);
std::expected<std::string, std::string> issue_jwt(const Application& application,
                                                  const ApplicationSigningKey& key,
                                                  const ApplicationUser& user,
                                                  std::string_view issuer,
                                                  int lifetime_seconds = 900,
                                                  std::string_view nonce = {});
std::string
webhook_signature(std::string_view secret, std::string_view event_id, std::string_view payload);

} // namespace apps

nlohmann::json to_json(const Application& value);
nlohmann::json to_json(const ApplicationUser& value);
nlohmann::json to_json(const ApplicationOrganization& value);
nlohmann::json to_json(const ApplicationWebhook& value);
nlohmann::json to_json(const ApplicationWebhookEvent& value);
nlohmann::json to_json(const ApplicationWebhookDelivery& value);

} // namespace lake::control
