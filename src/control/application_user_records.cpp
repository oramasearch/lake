#include "lake/control/application_user_records.hpp"

#include "lake/control/store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <utility>

#include <openssl/evp.h>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return ZettaControlResult<T>(
        std::unexpected(ZettaControlError{std::move(code), std::move(message)}));
}

std::string lowercase(std::string value) {
    std::ranges::transform(
        value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A record lake keeps in one of these collections is a user or organization, or a claim.
constexpr std::string_view claim_kind = "claim";

std::string now_instant() {
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return record_instant(now);
}

} // namespace

std::string application_directory_lookup(const std::string& application_id) {
    return "application:" + application_id;
}

std::string application_claims_lookup(const std::string& application_id) {
    return "application:" + application_id + "\nclaims";
}

std::string application_user_key(const WorkspaceRecords& records,
                                 const std::string& application_id,
                                 const std::string& user_id) {
    return records.index_of(ApplicationUserCodec::collection,
                            "application:" + application_id + "\nuser:" + user_id);
}

std::string application_claim_key(const WorkspaceRecords& records,
                                  RecordCollection collection,
                                  const std::string& application_id,
                                  std::string_view kind,
                                  const std::string& value) {
    const std::string compared = kind == email_claim ? lowercase(value) : value;
    return records.index_of(
        collection, "application:" + application_id + "\n" + std::string(kind) + ":" + compared);
}

// --- users -------------------------------------------------------------------------------------

std::string ApplicationUserCodec::lookup(const ApplicationUser& user) {
    return application_directory_lookup(user.application_id);
}

nlohmann::json ApplicationUserCodec::encode(const ApplicationUser& user) {
    return {{"kind", "user"},
            {"application_id", user.application_id},
            {"user_id", user.user_id},
            {"taap_id", user.taap_id},
            {"email", user.email},
            {"display_name", user.display_name},
            {"status", user.status},
            {"own_pool", user.own_pool},
            {"updated_at", user.updated_at}};
}

std::optional<ApplicationUser> ApplicationUserCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("kind", std::string()) != "user") {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationUser user;
    user.application_id = body.value("application_id", std::string());
    user.user_id = body.value("user_id", std::string());
    user.taap_id = body.value("taap_id", std::string());
    user.email = body.value("email", std::string());
    user.display_name = body.value("display_name", std::string());
    user.status = body.value("status", std::string());
    user.own_pool = body.value("own_pool", false);
    user.at_us = record.at_us;
    user.created_at = record_instant(record.at_us);
    user.updated_at = body.value("updated_at", std::string());
    if (user.updated_at.empty()) {
        user.updated_at = user.created_at;
    }
    user.revision = record.revision;
    user.lake_revision = record.lake_revision;
    if (user.application_id.empty() || user.user_id.empty() ||
        (user.status != "active" && user.status != "suspended")) {
        return std::nullopt;
    }
    return user;
}

// --- organizations -----------------------------------------------------------------------------

std::string ApplicationOrganizationCodec::lookup(const ApplicationOrganization& organization) {
    return application_directory_lookup(organization.application_id);
}

nlohmann::json ApplicationOrganizationCodec::encode(const ApplicationOrganization& organization) {
    return {{"kind", "organization"},
            {"application_id", organization.application_id},
            {"taap_id", organization.taap_id},
            {"name", organization.name},
            {"metadata", organization.metadata},
            {"updated_at", organization.updated_at}};
}

std::optional<ApplicationOrganization>
ApplicationOrganizationCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        record.body.value("kind", std::string()) != "organization") {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationOrganization organization;
    organization.id = record.id;
    organization.application_id = body.value("application_id", std::string());
    organization.taap_id = body.value("taap_id", std::string());
    organization.name = body.value("name", std::string());
    organization.metadata = body.value("metadata", nlohmann::json::object());
    organization.at_us = record.at_us;
    organization.created_at = record_instant(record.at_us);
    organization.updated_at = body.value("updated_at", std::string());
    if (organization.updated_at.empty()) {
        organization.updated_at = organization.created_at;
    }
    organization.revision = record.revision;
    organization.lake_revision = record.lake_revision;
    if (organization.application_id.empty() || organization.taap_id.empty() ||
        organization.name.empty() || !organization.metadata.is_object()) {
        return std::nullopt;
    }
    return organization;
}

// --- claims ------------------------------------------------------------------------------------

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationClaim>>>
read_application_claim(WorkspaceRecords& records, RecordCollection collection, std::string key) {
    using Found = std::optional<ApplicationClaim>;
    auto stored = settled(co_await records.get(collection, std::move(key)));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored || (*stored)->body.value("kind", std::string()) != claim_kind) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(ApplicationClaim{
        (*stored)->body.value("holder", std::string()), (*stored)->at_us, (*stored)->revision});
}

liquid::runtime::task<ZettaControlResult<ApplicationClaim>>
write_application_claim(WorkspaceRecords& records,
                        RecordCollection collection,
                        std::string application_id,
                        std::string key,
                        std::string holder,
                        std::optional<std::int64_t> over_revision) {
    RecordCondition condition;
    if (over_revision) {
        condition.if_revision = *over_revision;
    } else {
        condition.if_absent = true;
    }
    auto stored = settled(co_await records.put(
        collection,
        std::move(key),
        application_claims_lookup(application_id),
        {{"kind", claim_kind}, {"application_id", application_id}, {"holder", holder}},
        condition,
        over_revision ? 2 : 1));
    if (!stored) {
        co_return failed<ApplicationClaim>(stored.error());
    }
    co_return ZettaControlResult<ApplicationClaim>(
        ApplicationClaim{std::move(holder), stored->at_us, stored->revision});
}

liquid::runtime::task<ZettaControlResult<void>> release_application_claim(
    WorkspaceRecords& records, RecordCollection collection, std::string key, std::string holder) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto claim = settled(co_await read_application_claim(records, collection, key));
        if (!claim) {
            co_return failed<void>(claim.error());
        }
        if (!*claim || (*claim)->holder != holder) {
            co_return ZettaControlResult<void>();
        }
        auto removed = settled(co_await records.remove(collection, key, (*claim)->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return failed<void>(removed.error());
        }
    }
    co_return refused<void>("conflict", "the claim kept changing");
}

// --- user records ------------------------------------------------------------------------------

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationUser>>>
get_application_user(WorkspaceRecords& records, std::string application_id, std::string user_id) {
    using Found = std::optional<ApplicationUser>;
    auto stored = settled(co_await records.get(
        ApplicationUserCodec::collection, application_user_key(records, application_id, user_id)));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto user = ApplicationUserCodec::decode(**stored);
    if (!user || user->application_id != application_id || user->user_id != user_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(user));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationUser>>> list_application_users(
    WorkspaceRecords& records, std::string application_id, std::string query, std::size_t limit) {
    using Users = std::vector<ApplicationUser>;
    // Without a query only the newest `limit` are read; a search reads them all and filters.
    auto stored = settled(co_await records.list(ApplicationUserCodec::collection,
                                                application_directory_lookup(application_id),
                                                query.empty() ? limit : 0));
    if (!stored) {
        co_return failed<Users>(stored.error());
    }
    const std::string needle = lowercase(std::move(query));
    Users users;
    for (auto record = stored->rbegin(); record != stored->rend(); ++record) {
        auto user = ApplicationUserCodec::decode(*record);
        // A record whose key is not its user's was not written by lake for that user.
        if (!user || user->application_id != application_id ||
            application_user_key(records, application_id, user->user_id) != record->id) {
            continue;
        }
        if (!needle.empty() && !lowercase(user->email).contains(needle) &&
            !lowercase(user->display_name).contains(needle)) {
            continue;
        }
        users.push_back(std::move(*user));
        if (limit != 0 && users.size() >= limit) {
            break;
        }
    }
    co_return ZettaControlResult<Users>(std::move(users));
}

liquid::runtime::task<ZettaControlResult<ApplicationUser>>
create_application_user(WorkspaceRecords& records, ApplicationUser user) {
    if (user.application_id.empty() || user.user_id.empty()) {
        co_return refused<ApplicationUser>("invalid_input",
                                           "an application user needs its application and user");
    }
    if (user.status.empty()) {
        user.status = "active";
    }
    if (user.updated_at.empty() && user.at_us == 0) {
        user.updated_at = now_instant();
    }
    auto stored = settled(
        co_await records.put(ApplicationUserCodec::collection,
                             application_user_key(records, user.application_id, user.user_id),
                             ApplicationUserCodec::lookup(user),
                             ApplicationUserCodec::encode(user),
                             RecordCondition{.if_absent = true},
                             1,
                             user.at_us));
    if (!stored) {
        co_return failed<ApplicationUser>(stored.error());
    }
    auto decoded = ApplicationUserCodec::decode(*stored);
    if (!decoded) {
        co_return refused<ApplicationUser>("store_error", "the stored record did not decode");
    }
    co_return ZettaControlResult<ApplicationUser>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<ApplicationUser>>
set_application_user_status(WorkspaceRecords& records,
                            std::string application_id,
                            std::string user_id,
                            std::string status) {
    if (status != "active" && status != "suspended") {
        co_return refused<ApplicationUser>("invalid_input", "application user status is invalid");
    }
    ZettaControlError last{"conflict", "the application user kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto current = settled(co_await get_application_user(records, application_id, user_id));
        if (!current) {
            co_return failed<ApplicationUser>(current.error());
        }
        if (!*current) {
            co_return refused<ApplicationUser>("not_found", "application user does not exist");
        }
        ApplicationUser next = **current;
        next.status = status;
        next.updated_at = now_instant();
        auto written =
            settled(co_await records.put(ApplicationUserCodec::collection,
                                         application_user_key(records, application_id, user_id),
                                         ApplicationUserCodec::lookup(next),
                                         ApplicationUserCodec::encode(next),
                                         RecordCondition{.if_revision = next.revision},
                                         next.lake_revision + 1,
                                         next.at_us));
        if (written) {
            auto decoded = ApplicationUserCodec::decode(*written);
            if (!decoded) {
                co_return refused<ApplicationUser>("store_error",
                                                   "the stored record did not decode");
            }
            co_return ZettaControlResult<ApplicationUser>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return failed<ApplicationUser>(written.error());
        }
        last = written.error();
    }
    co_return failed<ApplicationUser>(last);
}

liquid::runtime::task<ZettaControlResult<void>> remove_application_user(WorkspaceRecords& records,
                                                                        ApplicationUser user) {
    auto removed = settled(
        co_await records.remove(ApplicationUserCodec::collection,
                                application_user_key(records, user.application_id, user.user_id),
                                user.revision));
    if (!removed && removed.error().code != "not_found") {
        co_return failed<void>(removed.error());
    }
    if (user.own_pool && !user.email.empty()) {
        co_return co_await release_application_claim(
            records,
            ApplicationUserCodec::collection,
            application_claim_key(records,
                                  ApplicationUserCodec::collection,
                                  user.application_id,
                                  email_claim,
                                  user.email),
            user.user_id);
    }
    co_return ZettaControlResult<void>();
}

// --- organization records ----------------------------------------------------------------------

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationOrganization>>>
get_application_organization(WorkspaceRecords& records,
                             std::string application_id,
                             std::string organization_id) {
    using Found = std::optional<ApplicationOrganization>;
    auto stored = settled(
        co_await records.get(ApplicationOrganizationCodec::collection, std::move(organization_id)));
    if (!stored) {
        if (stored.error().code == "invalid_input") {
            co_return ZettaControlResult<Found>(Found{}); // not an id lake could have made
        }
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto organization = ApplicationOrganizationCodec::decode(**stored);
    if (!organization || organization->application_id != application_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(organization));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationOrganization>>>
list_application_organizations(WorkspaceRecords& records,
                               std::string application_id,
                               std::size_t limit) {
    using Organizations = std::vector<ApplicationOrganization>;
    auto stored = settled(co_await records.list(ApplicationOrganizationCodec::collection,
                                                application_directory_lookup(application_id),
                                                limit));
    if (!stored) {
        co_return failed<Organizations>(stored.error());
    }
    Organizations organizations;
    for (auto record = stored->rbegin(); record != stored->rend(); ++record) {
        auto organization = ApplicationOrganizationCodec::decode(*record);
        if (organization && organization->application_id == application_id) {
            organizations.push_back(std::move(*organization));
        }
    }
    co_return ZettaControlResult<Organizations>(std::move(organizations));
}

liquid::runtime::task<ZettaControlResult<ApplicationOrganization>>
put_new_application_organization(WorkspaceRecords& records, ApplicationOrganization organization) {
    if (organization.id.empty()) {
        organization.id = new_uuid_v4();
    }
    if (organization.updated_at.empty() && organization.at_us == 0) {
        organization.updated_at = now_instant();
    }
    const std::string id = organization.id;
    auto stored = settled(co_await records.put(ApplicationOrganizationCodec::collection,
                                               id,
                                               ApplicationOrganizationCodec::lookup(organization),
                                               ApplicationOrganizationCodec::encode(organization),
                                               RecordCondition{.if_absent = true},
                                               1,
                                               organization.at_us));
    if (!stored) {
        co_return failed<ApplicationOrganization>(stored.error());
    }
    auto decoded = ApplicationOrganizationCodec::decode(*stored);
    if (!decoded) {
        co_return refused<ApplicationOrganization>("store_error",
                                                   "the stored record did not decode");
    }
    co_return ZettaControlResult<ApplicationOrganization>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<ApplicationOrganization>>
replace_application_organization(WorkspaceRecords& records, ApplicationOrganization next) {
    next.updated_at = now_instant();
    const std::string id = next.id;
    auto stored = settled(co_await records.put(ApplicationOrganizationCodec::collection,
                                               id,
                                               ApplicationOrganizationCodec::lookup(next),
                                               ApplicationOrganizationCodec::encode(next),
                                               RecordCondition{.if_revision = next.revision},
                                               next.lake_revision + 1,
                                               next.at_us));
    if (!stored) {
        co_return failed<ApplicationOrganization>(stored.error());
    }
    auto decoded = ApplicationOrganizationCodec::decode(*stored);
    if (!decoded) {
        co_return refused<ApplicationOrganization>("store_error",
                                                   "the stored record did not decode");
    }
    co_return ZettaControlResult<ApplicationOrganization>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<void>>
remove_application_organization(WorkspaceRecords& records, ApplicationOrganization organization) {
    const auto collection = ApplicationOrganizationCodec::collection;
    auto removed =
        settled(co_await records.remove(collection, organization.id, organization.revision));
    if (!removed && removed.error().code != "not_found") {
        co_return failed<void>(removed.error());
    }
    const std::array<std::pair<std::string_view, std::string>, 2> claims{
        std::pair{taap_claim, organization.taap_id}, std::pair{name_claim, organization.name}};
    for (const auto& [kind, value] : claims) {
        auto released = settled(co_await release_application_claim(
            records,
            collection,
            application_claim_key(records, collection, organization.application_id, kind, value),
            organization.id));
        if (!released) {
            co_return failed<void>(released.error());
        }
    }
    co_return ZettaControlResult<void>();
}

} // namespace lake::control
