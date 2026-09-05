#include "lake/control/memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace lake::control {
namespace {

std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

template <class T> void limit_rows(std::vector<T>& rows, int limit) {
    if (limit > 0 && rows.size() > static_cast<std::size_t>(limit)) {
        rows.resize(static_cast<std::size_t>(limit));
    }
}

} // namespace

Result<Application> InMemoryStore::create_application(const NewApplication& input) {
    std::lock_guard lock(mutex_);
    if (input.id.empty() || input.pool_id.empty() || input.customer_id.empty() ||
        input.name.empty() || input.allowed_origins.empty() || input.signing_key.seed.empty()) {
        return std::unexpected(StoreError{"invalid_input", "application fields are incomplete"});
    }
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "application customer does not exist"});
    }
    if (std::ranges::any_of(applications_, [&](const Application& value) {
            return value.id == input.id || value.pool_id == input.pool_id ||
                   (!input.custom_sign_in_domain.empty() &&
                    lowercase(value.custom_sign_in_domain) ==
                        lowercase(input.custom_sign_in_domain));
        })) {
        return std::unexpected(StoreError{"conflict", "application pool or domain already exists"});
    }
    const auto parent = std::ranges::find_if(policy_objects_, [&](const PolicyObject& value) {
        return value.object_type == "customer" && value.external_id == input.customer_id;
    });
    if (parent == policy_objects_.end()) {
        return std::unexpected(StoreError{"invalid_input", "customer policy object is missing"});
    }
    const std::string now = now_iso8601();
    applications_.push_back(Application{input.id,
                                        input.customer_id,
                                        input.pool_id,
                                        input.name,
                                        input.allowed_origins,
                                        input.redirect_uris,
                                        input.custom_sign_in_domain,
                                        input.theme,
                                        now,
                                        now});
    ApplicationSigningKey key = input.signing_key;
    key.application_id = input.id;
    key.current = true;
    key.created_at = now;
    application_signing_keys_.push_back(std::move(key));
    policy_objects_.push_back(
        PolicyObject{new_uuid_v4(), "application", input.id, parent->id, now, now, true});
    return applications_.back();
}

Result<std::vector<Application>> InMemoryStore::list_applications(const std::string& customer_id,
                                                                  int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Application> output;
    for (auto value = applications_.rbegin(); value != applications_.rend(); ++value) {
        if (customer_id.empty() || value->customer_id == customer_id) {
            output.push_back(*value);
        }
    }
    limit_rows(output, limit);
    return output;
}

Result<Application> InMemoryStore::get_application(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto value =
        std::ranges::find_if(applications_, [&](const Application& item) { return item.id == id; });
    if (value == applications_.end()) {
        return std::unexpected(StoreError{"not_found", "no application " + id});
    }
    return *value;
}

Result<Application> InMemoryStore::get_application_by_pool(const std::string& pool_id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(
        applications_, [&](const Application& item) { return item.pool_id == pool_id; });
    if (value == applications_.end()) {
        return std::unexpected(StoreError{"not_found", "no application pool " + pool_id});
    }
    return *value;
}

Result<Application>
InMemoryStore::get_application_by_domain(const std::string& custom_sign_in_domain) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(applications_, [&](const Application& item) {
        return !item.custom_sign_in_domain.empty() &&
               lowercase(item.custom_sign_in_domain) == lowercase(custom_sign_in_domain);
    });
    if (value == applications_.end()) {
        return std::unexpected(
            StoreError{"not_found", "no application domain " + custom_sign_in_domain});
    }
    return *value;
}

Result<Application> InMemoryStore::update_application_theme(const std::string& id,
                                                            const nlohmann::json& theme) {
    std::lock_guard lock(mutex_);
    const auto value =
        std::ranges::find_if(applications_, [&](const Application& item) { return item.id == id; });
    if (value == applications_.end()) {
        return std::unexpected(StoreError{"not_found", "no application " + id});
    }
    if (!theme.is_object()) {
        return std::unexpected(StoreError{"invalid_input", "application theme must be an object"});
    }
    value->theme = theme;
    value->updated_at = now_iso8601();
    return *value;
}

Result<ApplicationSigningKey>
InMemoryStore::rotate_application_signing_key(const ApplicationSigningKey& input) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(applications_, [&](const Application& value) {
            return value.id == input.application_id;
        })) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    const std::string now = now_iso8601();
    for (ApplicationSigningKey& value : application_signing_keys_) {
        if (value.application_id == input.application_id && value.current) {
            value.current = false;
            value.retired_at = now;
        }
    }
    ApplicationSigningKey value = input;
    value.current = true;
    value.created_at = now;
    application_signing_keys_.push_back(std::move(value));
    return application_signing_keys_.back();
}

Result<ApplicationSigningKey>
InMemoryStore::current_application_signing_key(const std::string& application_id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_signing_keys_, [&](const auto& item) {
        return item.application_id == application_id && item.current;
    });
    if (value == application_signing_keys_.end()) {
        return std::unexpected(StoreError{"not_found", "application has no current signing key"});
    }
    return *value;
}

Result<std::vector<ApplicationSigningKey>>
InMemoryStore::list_application_signing_keys(const std::string& application_id) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationSigningKey> output;
    for (const ApplicationSigningKey& value : application_signing_keys_) {
        if (value.application_id == application_id) {
            output.push_back(value);
        }
    }
    return output;
}

Result<ApplicationUser> InMemoryStore::register_application_user(const std::string& application_id,
                                                                 const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto existing =
        std::ranges::find_if(application_users_, [&](const ApplicationUser& item) {
            return item.application_id == application_id && item.user_id == user_id;
        });
    if (existing != application_users_.end()) {
        return *existing;
    }
    const auto user =
        std::ranges::find_if(users_, [&](const User& value) { return value.id == user_id; });
    if (user == users_.end()) {
        return std::unexpected(StoreError{"not_found", "application user does not exist"});
    }
    const std::string now = now_iso8601();
    application_users_.push_back(ApplicationUser{application_id,
                                                 user->id,
                                                 user->taap_id,
                                                 user->email,
                                                 user->display_name,
                                                 "active",
                                                 now,
                                                 now});
    return application_users_.back();
}

Result<ApplicationUser> InMemoryStore::get_application_user(const std::string& application_id,
                                                            const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_users_, [&](const ApplicationUser& item) {
        return item.application_id == application_id && item.user_id == user_id;
    });
    if (value == application_users_.end()) {
        return std::unexpected(StoreError{"not_found", "application user does not exist"});
    }
    return *value;
}

Result<std::vector<ApplicationUser>> InMemoryStore::list_application_users(
    const std::string& application_id, const std::string& query, int limit) {
    std::lock_guard lock(mutex_);
    const std::string needle = lowercase(query);
    std::vector<ApplicationUser> output;
    for (auto value = application_users_.rbegin(); value != application_users_.rend(); ++value) {
        if (value->application_id != application_id) {
            continue;
        }
        if (!needle.empty() && lowercase(value->email).find(needle) == std::string::npos &&
            lowercase(value->display_name).find(needle) == std::string::npos) {
            continue;
        }
        output.push_back(*value);
    }
    limit_rows(output, limit);
    return output;
}

Result<ApplicationUser> InMemoryStore::set_application_user_status(
    const std::string& application_id, const std::string& user_id, const std::string& status) {
    std::lock_guard lock(mutex_);
    if (status != "active" && status != "suspended") {
        return std::unexpected(StoreError{"invalid_input", "application user status is invalid"});
    }
    const auto value = std::ranges::find_if(application_users_, [&](const ApplicationUser& item) {
        return item.application_id == application_id && item.user_id == user_id;
    });
    if (value == application_users_.end()) {
        return std::unexpected(StoreError{"not_found", "application user does not exist"});
    }
    value->status = status;
    value->updated_at = now_iso8601();
    if (status == "suspended") {
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& item) { return item.id == application_id; });
        for (Session& session : sessions_) {
            if (application != applications_.end() && session.user_id == user_id &&
                session.pool_id == application->pool_id && session.revoked_at.empty()) {
                session.revoked_at = value->updated_at;
            }
        }
    }
    return *value;
}

Result<void> InMemoryStore::delete_application_user(const std::string& application_id,
                                                    const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto application = std::ranges::find_if(
        applications_, [&](const Application& item) { return item.id == application_id; });
    const auto value = std::ranges::find_if(application_users_, [&](const ApplicationUser& item) {
        return item.application_id == application_id && item.user_id == user_id;
    });
    if (application == applications_.end() || value == application_users_.end()) {
        return std::unexpected(StoreError{"not_found", "application user does not exist"});
    }
    const std::string pool_id = application->pool_id;
    application_users_.erase(value);
    std::erase_if(passwords_, [&](const PasswordCredential& item) {
        return item.user_id == user_id && item.pool_id == pool_id;
    });
    std::erase_if(passkeys_, [&](const PasskeyCredential& item) {
        return item.user_id == user_id && item.pool_id == pool_id;
    });
    std::erase_if(oauth_credentials_, [&](const OAuthCredential& item) {
        return item.user_id == user_id && item.pool_id == pool_id;
    });
    std::erase_if(sessions_, [&](const Session& item) {
        return item.user_id == user_id && item.pool_id == pool_id;
    });
    return {};
}

Result<ApplicationOrganization>
InMemoryStore::create_application_organization(const NewApplicationOrganization& input) {
    std::lock_guard lock(mutex_);
    if (input.application_id.empty() || input.taap_id.empty() || input.name.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "organization application, TAAP identity, and name are required"});
    }
    if (std::ranges::none_of(applications_,
                             [&](const auto& value) { return value.id == input.application_id; })) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    if (std::ranges::any_of(application_organizations_, [&](const auto& value) {
            return value.application_id == input.application_id &&
                   (value.taap_id == input.taap_id ||
                    lowercase(value.name) == lowercase(input.name));
        })) {
        return std::unexpected(StoreError{"conflict", "application organization already exists"});
    }
    const std::string now = now_iso8601();
    application_organizations_.push_back(ApplicationOrganization{
        new_uuid_v4(), input.application_id, input.taap_id, input.name, input.metadata, now, now});
    return application_organizations_.back();
}

Result<std::vector<ApplicationOrganization>>
InMemoryStore::list_application_organizations(const std::string& application_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationOrganization> output;
    for (auto value = application_organizations_.rbegin();
         value != application_organizations_.rend();
         ++value) {
        if (value->application_id == application_id) {
            output.push_back(*value);
        }
    }
    limit_rows(output, limit);
    return output;
}

Result<ApplicationOrganization> InMemoryStore::update_application_organization(
    const std::string& id, const std::string& name, const nlohmann::json& metadata) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_organizations_,
                                            [&](const auto& item) { return item.id == id; });
    if (value == application_organizations_.end()) {
        return std::unexpected(StoreError{"not_found", "application organization does not exist"});
    }
    if (!name.empty()) {
        value->name = name;
    }
    if (!metadata.is_null()) {
        value->metadata = metadata;
    }
    value->updated_at = now_iso8601();
    return *value;
}

Result<void> InMemoryStore::delete_application_organization(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_organizations_,
                                            [&](const auto& item) { return item.id == id; });
    if (value == application_organizations_.end()) {
        return std::unexpected(StoreError{"not_found", "application organization does not exist"});
    }
    application_organizations_.erase(value);
    return {};
}

Result<ApplicationWebhook>
InMemoryStore::create_application_webhook(const NewApplicationWebhook& input) {
    std::lock_guard lock(mutex_);
    if (input.application_id.empty() || input.url.empty() || input.events.empty() ||
        input.secret.empty()) {
        return std::unexpected(StoreError{"invalid_input", "webhook fields are incomplete"});
    }
    if (std::ranges::none_of(applications_,
                             [&](const auto& value) { return value.id == input.application_id; })) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    const std::string now = now_iso8601();
    application_webhooks_.push_back(ApplicationWebhook{new_uuid_v4(),
                                                       input.application_id,
                                                       input.url,
                                                       input.events,
                                                       input.secret,
                                                       true,
                                                       now,
                                                       now});
    return application_webhooks_.back();
}

Result<ApplicationWebhook> InMemoryStore::get_application_webhook(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_webhooks_,
                                            [&](const auto& item) { return item.id == id; });
    if (value == application_webhooks_.end()) {
        return std::unexpected(StoreError{"not_found", "application webhook does not exist"});
    }
    return *value;
}

Result<std::vector<ApplicationWebhook>>
InMemoryStore::list_application_webhooks(const std::string& application_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhook> output;
    for (auto value = application_webhooks_.rbegin(); value != application_webhooks_.rend();
         ++value) {
        if (value->application_id == application_id) {
            output.push_back(*value);
        }
    }
    limit_rows(output, limit);
    return output;
}

Result<ApplicationWebhookEvent> InMemoryStore::create_application_webhook_event(
    const std::string& application_id, const std::string& event, const nlohmann::json& payload) {
    std::lock_guard lock(mutex_);
    if (std::ranges::none_of(applications_,
                             [&](const auto& value) { return value.id == application_id; })) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    application_webhook_events_.push_back(
        ApplicationWebhookEvent{new_uuid_v4(), application_id, event, payload, now_iso8601()});
    return application_webhook_events_.back();
}

Result<ApplicationWebhookEvent>
InMemoryStore::get_application_webhook_event(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto value = std::ranges::find_if(application_webhook_events_,
                                            [&](const auto& item) { return item.id == id; });
    if (value == application_webhook_events_.end()) {
        return std::unexpected(StoreError{"not_found", "application webhook event does not exist"});
    }
    return *value;
}

Result<std::vector<ApplicationWebhookEvent>>
InMemoryStore::list_application_webhook_events(const std::string& application_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhookEvent> output;
    for (auto value = application_webhook_events_.rbegin();
         value != application_webhook_events_.rend();
         ++value) {
        if (value->application_id == application_id) {
            output.push_back(*value);
        }
    }
    limit_rows(output, limit);
    return output;
}

Result<ApplicationWebhookDelivery>
InMemoryStore::create_application_webhook_delivery(const NewApplicationWebhookDelivery& input) {
    std::lock_guard lock(mutex_);
    application_webhook_deliveries_.push_back(ApplicationWebhookDelivery{new_uuid_v4(),
                                                                         input.event_id,
                                                                         input.webhook_id,
                                                                         input.attempt,
                                                                         input.status,
                                                                         input.signature,
                                                                         input.error,
                                                                         now_iso8601()});
    return application_webhook_deliveries_.back();
}

Result<std::vector<ApplicationWebhookDelivery>>
InMemoryStore::list_application_webhook_deliveries(const std::string& event_id) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhookDelivery> output;
    for (const auto& value : application_webhook_deliveries_) {
        if (value.event_id == event_id) {
            output.push_back(value);
        }
    }
    return output;
}

} // namespace lake::control
