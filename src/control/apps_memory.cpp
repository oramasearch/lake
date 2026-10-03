#include "lake/control/memory_store.hpp"

#include "lake/control/application_user_records.hpp"
#include "secrets_crypto.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ranges>

namespace lake::control {
namespace {

// The credential purposes the envelope serves (Store::encrypt_application_credential).
bool application_credential_purpose(std::string_view purpose) {
    return purpose.starts_with("application.jwt.") || purpose.starts_with("application.webhook.");
}

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
    if (input.id.empty() || input.pool_id.empty() || input.customer_id.empty()) {
        return std::unexpected(StoreError{"invalid_input", "application fields are incomplete"});
    }
    const auto customer = std::ranges::find_if(
        customers_, [&](const Customer& value) { return value.id == input.customer_id; });
    if (customer == customers_.end()) {
        return std::unexpected(StoreError{"not_found", "application customer does not exist"});
    }
    // As the routing trigger of migration 0049: a live workspace of the application's own space.
    if (std::ranges::none_of(workspaces_, [&](const Workspace& value) {
            return value.id == input.workspace_id && value.customer_id == input.customer_id &&
                   value.desired != "deleted";
        })) {
        return std::unexpected(
            StoreError{"invalid_input", "workspace_id must name a workspace in this space"});
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
    // The routing row only: the settings are the application's record in its workspace.
    Application application;
    application.id = input.id;
    application.customer_id = input.customer_id;
    application.pool_id = input.pool_id;
    application.custom_sign_in_domain = input.custom_sign_in_domain;
    application.created_at = now;
    application.updated_at = now;
    application.workspace_id = input.workspace_id;
    applications_.push_back(std::move(application));
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

void InMemoryStore::seed_legacy_application_settings(ApplicationSettings settings) {
    std::lock_guard lock(mutex_);
    legacy_application_settings_.push_back(std::move(settings));
}

// As migration 0059: each application's settings belong to its workspace, are needs_selection
// while it has none (and are resolved once it has a live one), and workspace_deleted when that
// workspace is gone.
Result<void> InMemoryStore::assign_legacy_application_settings() {
    std::lock_guard lock(mutex_);
    for (const ApplicationSettings& settings : legacy_application_settings_) {
        const auto application = std::ranges::find_if(applications_, [&](const Application& value) {
            return value.id == settings.application_id;
        });
        if (application == applications_.end()) {
            continue;
        }
        auto& assigned = legacy_application_settings_assignments_[settings.application_id];
        if (!assigned.first.empty() || assigned.second == "workspace_deleted") {
            continue;
        }
        const auto workspace = std::ranges::find_if(workspaces_, [&](const Workspace& value) {
            return value.id == application->workspace_id;
        });
        if (application->workspace_id.empty()) {
            assigned = {"", "needs_selection"};
        } else if (workspace != workspaces_.end() && workspace->desired == "deleted") {
            assigned = {"", assigned.second.empty() ? "workspace_deleted" : assigned.second};
        } else {
            assigned = {application->workspace_id, "application_workspace"};
        }
    }
    return {};
}

Result<std::vector<LegacyApplicationSettings>>
InMemoryStore::legacy_application_settings(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationSettings> ordered = legacy_application_settings_;
    std::ranges::sort(ordered, {}, &ApplicationSettings::application_id);
    std::vector<LegacyApplicationSettings> rows;
    for (const ApplicationSettings& settings : ordered) {
        if ((!after_id.empty() && settings.application_id <= after_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        const auto application = std::ranges::find_if(applications_, [&](const Application& value) {
            return value.id == settings.application_id;
        });
        if (application == applications_.end()) {
            continue; // deleted with its space: nothing to copy
        }
        LegacyApplicationSettings row{settings, application->customer_id, "", "needs_selection"};
        row.settings.customer_id = application->customer_id;
        if (const auto found = legacy_application_settings_assignments_.find(settings.application_id);
            found != legacy_application_settings_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<std::string> InMemoryStore::encrypt_application_credential(const std::string& purpose,
                                                                  const std::string& plaintext) {
    std::lock_guard lock(mutex_);
    if (!application_credential_purpose(purpose) || plaintext.empty()) {
        return std::unexpected(StoreError{"invalid_input", "not an application credential"});
    }
    auto& data_key = application_credential_keys_[purpose];
    if (data_key.empty()) {
        auto generated = secrets_crypto::random_key();
        if (!generated) {
            return std::unexpected(generated.error());
        }
        data_key.assign(reinterpret_cast<const char*>(generated->data()), generated->size());
        secrets_crypto::cleanse(*generated);
    }
    secrets_crypto::Key key{};
    std::copy(data_key.begin(), data_key.end(), key.begin());
    auto sealed = secrets_crypto::seal(key, plaintext, purpose);
    secrets_crypto::cleanse(key);
    return sealed;
}

Result<std::string> InMemoryStore::decrypt_application_credential(const std::string& purpose,
                                                                  const std::string& envelope) {
    std::lock_guard lock(mutex_);
    const auto data_key = application_credential_keys_.find(purpose);
    if (!application_credential_purpose(purpose) || data_key == application_credential_keys_.end()) {
        return std::unexpected(StoreError{"store_error", "encrypted credential cannot be opened"});
    }
    secrets_crypto::Key key{};
    std::copy(data_key->second.begin(), data_key->second.end(), key.begin());
    auto opened = secrets_crypto::open(key, envelope, purpose);
    secrets_crypto::cleanse(key);
    return opened;
}

Result<int> InMemoryStore::revoke_application_user_sessions(const std::string& application_id,
                                                            const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto application = std::ranges::find_if(
        applications_, [&](const Application& item) { return item.id == application_id; });
    if (application == applications_.end()) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    const std::string now = now_iso8601();
    int revoked = 0;
    for (Session& session : sessions_) {
        if (session.user_id == user_id && session.pool_id == application->pool_id &&
            session.revoked_at.empty()) {
            session.revoked_at = now;
            ++revoked;
        }
    }
    return revoked;
}

Result<void> InMemoryStore::delete_application_user_credentials(const std::string& application_id,
                                                                const std::string& user_id) {
    std::lock_guard lock(mutex_);
    const auto application = std::ranges::find_if(
        applications_, [&](const Application& item) { return item.id == application_id; });
    if (application == applications_.end()) {
        return std::unexpected(StoreError{"not_found", "application does not exist"});
    }
    const std::string pool_id = application->pool_id;
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

void InMemoryStore::seed_legacy_application_user(ApplicationUser user) {
    std::lock_guard lock(mutex_);
    legacy_application_users_.push_back(std::move(user));
}

void InMemoryStore::seed_legacy_application_organization(ApplicationOrganization organization) {
    std::lock_guard lock(mutex_);
    if (organization.id.empty()) {
        organization.id = new_uuid_v4();
    }
    legacy_application_organizations_.push_back(std::move(organization));
}

Result<void> InMemoryStore::assign_legacy_application_directory() {
    std::lock_guard lock(mutex_);
    const auto assign = [&](const std::string& row_id, const std::string& application_id) {
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& value) { return value.id == application_id; });
        if (application == applications_.end()) {
            return;
        }
        auto& assigned = legacy_application_directory_assignments_[row_id];
        if (assigned.first.empty()) {
            assigned = application->workspace_id.empty()
                           ? std::pair<std::string, std::string>{"", "needs_selection"}
                           : std::pair<std::string, std::string>{application->workspace_id,
                                                                 "application_workspace"};
        }
    };
    for (const ApplicationUser& user : legacy_application_users_) {
        assign(application_user_row_id(user.application_id, user.user_id), user.application_id);
    }
    for (const ApplicationOrganization& organization : legacy_application_organizations_) {
        assign(organization.id, organization.application_id);
    }
    return {};
}

Result<std::vector<LegacyApplicationUser>>
InMemoryStore::legacy_application_users(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<LegacyApplicationUser> rows;
    for (const ApplicationUser& user : legacy_application_users_) {
        LegacyApplicationUser row{application_user_row_id(user.application_id, user.user_id),
                                  user,
                                  "",
                                  "",
                                  "needs_selection"};
        if (!after_id.empty() && row.id <= after_id) {
            continue;
        }
        const auto application = std::ranges::find_if(applications_, [&](const Application& value) {
            return value.id == user.application_id;
        });
        if (application != applications_.end()) {
            row.customer_id = application->customer_id;
        }
        if (const auto found = legacy_application_directory_assignments_.find(row.id);
            found != legacy_application_directory_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    std::ranges::sort(rows, {}, &LegacyApplicationUser::id);
    if (rows.size() > static_cast<std::size_t>(std::clamp(limit, 1, 5000))) {
        rows.resize(static_cast<std::size_t>(std::clamp(limit, 1, 5000)));
    }
    return rows;
}

Result<std::vector<LegacyApplicationOrganization>>
InMemoryStore::legacy_application_organizations(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationOrganization> ordered = legacy_application_organizations_;
    std::ranges::sort(ordered, {}, &ApplicationOrganization::id);
    std::vector<LegacyApplicationOrganization> rows;
    for (const ApplicationOrganization& organization : ordered) {
        if ((!after_id.empty() && organization.id <= after_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        LegacyApplicationOrganization row{organization, "", "", "needs_selection"};
        const auto application = std::ranges::find_if(applications_, [&](const Application& value) {
            return value.id == organization.application_id;
        });
        if (application != applications_.end()) {
            row.customer_id = application->customer_id;
        }
        if (const auto found = legacy_application_directory_assignments_.find(organization.id);
            found != legacy_application_directory_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void InMemoryStore::seed_legacy_application_signing_key(ApplicationSigningKey key) {
    auto envelope = encrypt_application_credential("application.jwt." + key.application_id, key.seed);
    std::lock_guard lock(mutex_);
    key.encrypted_seed = envelope ? *envelope : std::string();
    key.seed.clear();
    application_signing_keys_.push_back(std::move(key));
}

void InMemoryStore::seed_legacy_application_webhook(ApplicationWebhook webhook) {
    if (webhook.id.empty()) {
        webhook.id = new_uuid_v4();
    }
    if (webhook.at_us == 0) {
        webhook.at_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    }
    auto envelope = encrypt_application_credential("application.webhook." + webhook.id, webhook.secret);
    std::lock_guard lock(mutex_);
    webhook.encrypted_secret = envelope ? *envelope : std::string();
    webhook.secret.clear();
    application_webhooks_.push_back(std::move(webhook));
}

// As migration 0056: each key ring and webhook belongs to its application's workspace, or is
// needs_selection while the application has none (and is resolved once it has one).
Result<void> InMemoryStore::assign_legacy_application_credentials() {
    std::lock_guard lock(mutex_);
    const auto assign = [&](const std::string& table, const std::string& id,
                            const std::string& application_id) {
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& value) { return value.id == application_id; });
        if (application == applications_.end()) {
            return;
        }
        auto& assigned = legacy_application_credential_assignments_[table + "\n" + id];
        if (!assigned.first.empty() || assigned.second == "workspace_deleted") {
            return;
        }
        const bool deleted = std::ranges::any_of(workspaces_, [&](const Workspace& value) {
            return value.id == application->workspace_id && value.desired == "deleted";
        });
        if (application->workspace_id.empty()) {
            assigned = {"", "needs_selection"};
        } else if (deleted) {
            assigned = {"", assigned.second.empty() ? "workspace_deleted" : assigned.second};
        } else {
            assigned = {application->workspace_id, "application_workspace"};
        }
    };
    for (const ApplicationSigningKey& key : application_signing_keys_) {
        assign("auth_app_signing_keys", key.application_id, key.application_id);
    }
    for (const ApplicationWebhook& webhook : application_webhooks_) {
        assign("auth_app_webhooks", webhook.id, webhook.application_id);
    }
    return {};
}

Result<std::vector<LegacyApplicationSigningKeyRing>>
InMemoryStore::legacy_application_signing_keys(const std::string& after_application_id, int limit) {
    std::lock_guard lock(mutex_);
    std::map<std::string, ApplicationSigningKeyRing> rings;
    for (const ApplicationSigningKey& key : application_signing_keys_) {
        auto& ring = rings[key.application_id];
        ring.application_id = key.application_id;
        ring.keys.push_back(key);
    }
    std::vector<LegacyApplicationSigningKeyRing> rows;
    for (auto& [application_id, ring] : rings) {
        if ((!after_application_id.empty() && application_id <= after_application_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        std::ranges::sort(ring.keys, {}, &ApplicationSigningKey::version);
        LegacyApplicationSigningKeyRing row{std::move(ring), "", "", "needs_selection"};
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& value) { return value.id == application_id; });
        if (application != applications_.end()) {
            row.customer_id = application->customer_id;
        }
        if (const auto found = legacy_application_credential_assignments_.find(
                "auth_app_signing_keys\n" + application_id);
            found != legacy_application_credential_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<std::vector<LegacyApplicationWebhook>>
InMemoryStore::legacy_application_webhooks(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhook> ordered = application_webhooks_;
    std::ranges::sort(ordered, {}, &ApplicationWebhook::id);
    std::vector<LegacyApplicationWebhook> rows;
    for (const ApplicationWebhook& webhook : ordered) {
        if ((!after_id.empty() && webhook.id <= after_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        LegacyApplicationWebhook row{webhook, "", "", "needs_selection"};
        const auto application = std::ranges::find_if(applications_, [&](const Application& value) {
            return value.id == webhook.application_id;
        });
        if (application != applications_.end()) {
            row.customer_id = application->customer_id;
        }
        if (const auto found =
                legacy_application_credential_assignments_.find("auth_app_webhooks\n" + webhook.id);
            found != legacy_application_credential_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void InMemoryStore::seed_legacy_application_webhook_event(ApplicationWebhookEvent event) {
    std::lock_guard lock(mutex_);
    if (event.id.empty()) {
        event.id = new_uuid_v4();
    }
    application_webhook_events_.push_back(std::move(event));
}

void InMemoryStore::seed_legacy_application_webhook_delivery(ApplicationWebhookDelivery delivery) {
    std::lock_guard lock(mutex_);
    if (delivery.id.empty()) {
        delivery.id = new_uuid_v4();
    }
    application_webhook_deliveries_.push_back(std::move(delivery));
}

Result<void> InMemoryStore::assign_legacy_application_webhook_events() {
    std::lock_guard lock(mutex_);
    for (const ApplicationWebhookEvent& event : application_webhook_events_) {
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& value) { return value.id == event.application_id; });
        if (application == applications_.end()) {
            continue;
        }
        auto& assigned = legacy_application_event_assignments_[event.id];
        if (assigned.first.empty()) {
            assigned = application->workspace_id.empty()
                ? std::pair<std::string, std::string>{"", "needs_selection"}
                : std::pair<std::string, std::string>{application->workspace_id,
                                                      "application_workspace"};
        }
    }
    return {};
}

Result<std::vector<LegacyApplicationWebhookEvent>>
InMemoryStore::legacy_application_webhook_events(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhookEvent> ordered = application_webhook_events_;
    std::ranges::sort(ordered, {}, &ApplicationWebhookEvent::id);
    std::vector<LegacyApplicationWebhookEvent> rows;
    for (const ApplicationWebhookEvent& event : ordered) {
        if ((!after_id.empty() && event.id <= after_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        LegacyApplicationWebhookEvent row{event, "", "", "needs_selection"};
        const auto application = std::ranges::find_if(
            applications_, [&](const Application& value) { return value.id == event.application_id; });
        if (application != applications_.end()) {
            row.customer_id = application->customer_id;
        }
        if (const auto found = legacy_application_event_assignments_.find(event.id);
            found != legacy_application_event_assignments_.end()) {
            row.workspace_id = found->second.first;
            row.outcome = found->second.second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<std::vector<LegacyApplicationWebhookDelivery>>
InMemoryStore::legacy_application_webhook_deliveries(const std::string& after_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<ApplicationWebhookDelivery> ordered = application_webhook_deliveries_;
    std::ranges::sort(ordered, {}, &ApplicationWebhookDelivery::id);
    std::vector<LegacyApplicationWebhookDelivery> rows;
    for (ApplicationWebhookDelivery delivery : ordered) {
        if ((!after_id.empty() && delivery.id <= after_id) ||
            static_cast<int>(rows.size()) >= std::clamp(limit, 1, 5000)) {
            continue;
        }
        LegacyApplicationWebhookDelivery row{{}, "", "", "needs_selection"};
        const auto event = std::ranges::find_if(application_webhook_events_, [&](const auto& value) {
            return value.id == delivery.event_id;
        });
        if (event != application_webhook_events_.end()) {
            delivery.application_id = event->application_id;
            const auto application = std::ranges::find_if(applications_, [&](const auto& value) {
                return value.id == event->application_id;
            });
            if (application != applications_.end()) {
                row.customer_id = application->customer_id;
            }
            if (const auto found = legacy_application_event_assignments_.find(event->id);
                found != legacy_application_event_assignments_.end()) {
                row.workspace_id = found->second.first;
                row.outcome = found->second.second;
            }
        }
        row.delivery = std::move(delivery);
        rows.push_back(std::move(row));
    }
    return rows;
}

Result<void>
InMemoryStore::create_application_authorization_code(const ApplicationAuthorizationCode& value) {
    if (value.application_id.empty() || value.user_id.empty() || value.redirect_uri.empty() ||
        value.code_hash.empty() || value.code_challenge.empty() || value.nonce.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "authorization-code fields are required"});
    }
    std::lock_guard lock(mutex_);
    if (std::ranges::any_of(application_authorization_codes_, [&](const auto& candidate) {
            return candidate.code.code_hash == value.code_hash;
        })) {
        return std::unexpected(StoreError{"conflict", "authorization code already exists"});
    }
    application_authorization_codes_.push_back(
        {value, std::chrono::steady_clock::now() + std::chrono::minutes(5), false});
    return {};
}

Result<ApplicationAuthorizationCode>
InMemoryStore::consume_application_authorization_code(const std::string& application_id,
                                                      const std::string& code_hash) {
    std::lock_guard lock(mutex_);
    for (auto& candidate : application_authorization_codes_) {
        if (candidate.code.application_id == application_id &&
            candidate.code.code_hash == code_hash && !candidate.used &&
            candidate.expires_at > std::chrono::steady_clock::now()) {
            candidate.used = true;
            return candidate.code;
        }
    }
    return std::unexpected(
        StoreError{"unauthorized", "authorization code is invalid, expired, or already used"});
}

} // namespace lake::control
