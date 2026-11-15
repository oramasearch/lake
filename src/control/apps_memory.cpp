// SPDX-License-Identifier: MIT

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
            StoreError{"invalid_input", "workspace_id must name a workspace in this account"});
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

namespace {

std::int64_t memory_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

} // namespace lake::control
