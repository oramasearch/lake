#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

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
    // The workspace whose Zetta holds the application's lifecycle events and delivery attempts
    // (lake#356, lake#357). Set at registration and never changed; empty only for an application
    // registered before migration 0049 whose space had several workspaces, until it is assigned.
    std::string workspace_id;
};

struct ApplicationSigningKey {
    std::string application_id;
    std::string kid;
    std::string seed;
    std::string public_key;
    int version = 0;
    bool current = false;
    std::string created_at;
    std::string retired_at;
};

struct NewApplication {
    std::string id;
    std::string customer_id;
    std::string pool_id;
    std::string name;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> redirect_uris;
    std::string custom_sign_in_domain;
    nlohmann::json theme = nlohmann::json::object();
    ApplicationSigningKey signing_key;
    std::string workspace_id; // a live workspace of the customer (migration 0049)
};

struct ApplicationUser {
    std::string application_id;
    std::string user_id;
    std::string taap_id;
    std::string email;
    std::string display_name;
    std::string status;
    std::string created_at;
    std::string updated_at;
};

struct ApplicationOrganization {
    std::string id;
    std::string application_id;
    std::string taap_id;
    std::string name;
    nlohmann::json metadata = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
};

struct NewApplicationOrganization {
    std::string application_id;
    std::string taap_id;
    std::string name;
    nlohmann::json metadata = nlohmann::json::object();
};

struct ApplicationWebhook {
    std::string id;
    std::string application_id;
    std::string url;
    std::vector<std::string> events;
    std::string secret;
    bool enabled = true;
    std::string created_at;
    std::string updated_at;
};

struct NewApplicationWebhook {
    std::string application_id;
    std::string url;
    std::vector<std::string> events;
    std::string secret;
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

struct ApplicationAuthorizationCode {
    std::string application_id;
    std::string user_id;
    std::string redirect_uri;
    std::string code_hash;
    std::string code_challenge;
    std::string nonce;
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
