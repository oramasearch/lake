#pragma once

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

struct ApplicationWebhookEvent {
    std::string id;
    std::string application_id;
    std::string event;
    nlohmann::json payload = nlohmann::json::object();
    std::string created_at;
};

struct ApplicationWebhookDelivery {
    std::string id;
    std::string event_id;
    std::string webhook_id;
    int attempt = 0;
    int status = 0;
    std::string signature;
    std::string error;
    std::string created_at;
};

struct NewApplicationWebhookDelivery {
    std::string event_id;
    std::string webhook_id;
    int attempt = 0;
    int status = 0;
    std::string signature;
    std::string error;
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
