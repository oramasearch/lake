#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <liquid/liquid.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/search/index.hpp"
#include "temporary_directory.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

class TemporarySearchStore {
public:
    TemporarySearchStore() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("postgres-search-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporarySearchStore() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> search_bytes(const std::string& value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::string create_search_change(const std::filesystem::path& path, const std::string& content) {
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(path.parent_path().parent_path() / "store"));
    EXPECT_TRUE(customer_store) << customer_store.error().message;
    if (!customer_store) {
        return {};
    }
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::create(path, options));
    EXPECT_TRUE(opened) << opened.error().message;
    if (!opened) {
        return {};
    }
    auto repository = std::move(*opened);
    const auto written =
        liquid::runtime::block_on(repository.write("document.txt", search_bytes(content)));
    EXPECT_TRUE(written) << written.error().message;
    const auto change = liquid::runtime::block_on(repository.cut("search fixture"));
    EXPECT_TRUE(change) << change.error().message;
    return change ? liquid::to_string(change->value) : "";
}

TEST(PostgresCapabilities, OrganizationAndPolicyRoundTrip) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.acting_identity = "admin";
    context.request_id = "postgres-integration";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto call = [&](const std::string& name, json input = json::object()) {
        return registry.call(name, input, context);
    };
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

    ASSERT_TRUE(call("server_info").ok);
    const ApiResult first_user = call("create_user",
                                      {{"taap_id", "taap:1:ed25519:first-" + suffix},
                                       {"email", "first-" + suffix + "@example.com"},
                                       {"display_name", "First"}});
    const ApiResult second_user = call("create_user",
                                       {{"taap_id", "taap:1:ed25519:second-" + suffix},
                                        {"email", "second-" + suffix + "@example.com"},
                                        {"display_name", "Second"}});
    ASSERT_TRUE(first_user.ok) << first_user.error.message;
    ASSERT_TRUE(second_user.ok) << second_user.error.message;
    ASSERT_TRUE(call("list_users").ok);
    ASSERT_TRUE(call("get_user", {{"id", first_user.value.at("id")}}).ok);

    const ApiResult organization = call("create_organization",
                                        {{"taap_id", "taap:1:ed25519:org-" + suffix},
                                         {"display_name", "Integration organization"}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    ASSERT_TRUE(call("list_organizations").ok);
    ASSERT_TRUE(call("get_organization", {{"id", organization.value.at("id")}}).ok);

    const ApiResult customer = call("create_customer",
                                    {{"customer_type", "organization"},
                                     {"organization_id", organization.value.at("id")},
                                     {"name", "org-" + suffix}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    ASSERT_TRUE(call("list_customers").ok);
    ASSERT_TRUE(call("get_customer", {{"id", customer.value.at("id")}}).ok);

    const ApiResult repository =
        call("create_repository",
             {{"customer_id", customer.value.at("id")}, {"name", "repo-" + suffix}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    ASSERT_TRUE(call("list_repositories", {{"customer_id", customer.value.at("id")}}).ok);
    ASSERT_TRUE(call("get_repository", {{"id", repository.value.at("id")}}).ok);

    const ApiResult destination_user =
        call("create_user", {{"display_name", "Repository destination"}});
    ASSERT_TRUE(destination_user.ok) << destination_user.error.message;
    const ApiResult destination = call("create_customer",
                                       {{"customer_type", "individual"},
                                        {"user_id", destination_user.value.at("id")},
                                        {"name", "destination-" + suffix}});
    ASSERT_TRUE(destination.ok) << destination.error.message;
    ASSERT_TRUE(call("set_repository_visibility",
                     {{"id", repository.value.at("id")}, {"visibility", "organization"}})
                    .ok);
    ASSERT_TRUE(call("rename_repository",
                     {{"id", repository.value.at("id")}, {"name", "renamed-" + suffix}})
                    .ok);
    const ApiResult transferred =
        call("transfer_repository",
             {{"id", repository.value.at("id")}, {"customer_id", destination.value.at("id")}});
    ASSERT_TRUE(transferred.ok) << transferred.error.message;
    EXPECT_EQ(transferred.value.at("customer_id"), destination.value.at("id"));
    const ApiResult deleted = call("delete_repository", {{"id", repository.value.at("id")}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_FALSE(deleted.value.at("deleted_at").is_null());
    EXPECT_EQ(
        call("rename_repository", {{"id", repository.value.at("id")}, {"name", "again-" + suffix}})
            .error.code,
        "conflict");
    const ApiResult audit = call("audit", {{"limit", 1000}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    EXPECT_TRUE(std::ranges::any_of(audit.value.at("items"), [](const json& event) {
        return event.at("action") == "delete_repository" && event.at("outcome") == "success";
    }));

    const ApiResult team = call("create_team",
                                {{"organization_id", organization.value.at("id")},
                                 {"name", "team-" + suffix},
                                 {"display_name", "Integration team"}});
    ASSERT_TRUE(team.ok) << team.error.message;
    ASSERT_TRUE(call("list_teams", {{"organization_id", organization.value.at("id")}}).ok);
    ASSERT_TRUE(call("get_team", {{"id", team.value.at("id")}}).ok);

    const ApiResult membership = call("create_membership",
                                      {{"organization_id", organization.value.at("id")},
                                       {"principal_type", "user"},
                                       {"principal_id", first_user.value.at("id")},
                                       {"principal_taap_id", first_user.value.at("taap_id")},
                                       {"role", "owner"},
                                       {"attestation_digest", "sha512-256:owner-" + suffix}});
    ASSERT_TRUE(membership.ok) << membership.error.message;
    ASSERT_TRUE(call("list_memberships", {{"organization_id", organization.value.at("id")}}).ok);

    const ApiResult invitation = call("create_invitation",
                                      {{"organization_id", organization.value.at("id")},
                                       {"team_id", team.value.at("id")},
                                       {"invitee_type", "email"},
                                       {"invitee", "second-" + suffix + "@example.com"},
                                       {"role", "member"},
                                       {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(invitation.ok) << invitation.error.message;
    ASSERT_TRUE(
        call("list_invitations",
             {{"organization_id", organization.value.at("id")}, {"team_id", team.value.at("id")}})
            .ok);
    const ApiResult accepted = call("accept_invitation",
                                    {{"invitation_id", invitation.value.at("id")},
                                     {"principal_type", "user"},
                                     {"principal_id", second_user.value.at("id")},
                                     {"principal_taap_id", second_user.value.at("taap_id")},
                                     {"attestation_digest", "sha512-256:accepted-" + suffix}});
    ASSERT_TRUE(accepted.ok) << accepted.error.message;

    const auto repository_object =
        store->find_policy_object("repository", repository.value.at("id"));
    ASSERT_TRUE(repository_object) << repository_object.error().message;
    const ApiResult path = call("create_object",
                                {{"object_type", "path"},
                                 {"external_id", "node-" + suffix},
                                 {"parent_id", repository_object->id},
                                 {"inherit", true}});
    ASSERT_TRUE(path.ok) << path.error.message;
    ASSERT_TRUE(call("get_object", {{"id", path.value.at("id")}}).ok);
    ASSERT_TRUE(call("list_objects", {{"parent_id", repository_object->id}}).ok);

    const ApiResult grant = call("create_grant",
                                 {{"principal_taap_id", "taap:1:ed25519:reader-" + suffix},
                                  {"object_id", repository_object->id},
                                  {"role", "read"},
                                  {"attestation_digest", "sha512-256:grant-" + suffix}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(call("list_grants", {{"object_id", repository_object->id}}).ok);
    const ApiResult access = call("effective_access", {{"object_id", path.value.at("id")}});
    ASSERT_TRUE(access.ok) << access.error.message;
    EXPECT_FALSE(access.value.at("items").empty());
}

TEST(PostgresCapabilities, DriveShareLinksRoundTrip) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string node_id = suffix + "000000000000000000000000";
    const auto user = store->create_user(
        {"taap:1:ed25519:drive-" + suffix, "drive-" + suffix + "@example.test", "Drive"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "drive-" + suffix});
    ASSERT_TRUE(customer);
    const auto repository =
        store->create_repository({customer->id, "drive-" + suffix, "", "private"});
    ASSERT_TRUE(repository);
    const auto repository_object = store->find_policy_object("repository", repository->id);
    ASSERT_TRUE(repository_object);
    const auto folder = store->create_policy_object({"path",
                                                     repository->id + ":" + node_id,
                                                     repository_object->id,
                                                     user->taap_id,
                                                     "request-folder-" + suffix,
                                                     true});
    ASSERT_TRUE(folder);
    const auto link = store->create_drive_share_link({repository->id,
                                                      folder->id,
                                                      node_id,
                                                      "shared",
                                                      "read",
                                                      "token-hash-" + suffix,
                                                      "2999-01-01T00:00:00Z",
                                                      user->taap_id,
                                                      "request-link-" + suffix});
    ASSERT_TRUE(link) << link.error().message;
    EXPECT_EQ(store->get_drive_share_link(link->id)->path, "shared");
    EXPECT_EQ(store->get_drive_share_link_by_token_hash("token-hash-" + suffix)->id, link->id);
    const auto listed = store->list_drive_share_links(repository->id, folder->id, 10);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1u);
    const auto revoked = store->revoke_drive_share_link(link->id);
    ASSERT_TRUE(revoked);
    EXPECT_FALSE(revoked->revoked_at.empty());
}

TEST(PostgresCapabilities, SearchFiltersRepositoryCandidates) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const std::string customer = "search-customer-" + suffix;
    const std::string allowed_repository = "allowed-" + suffix;
    const std::string denied_repository = "denied-" + suffix;
    const auto customer_object = store->create_policy_object(
        {"customer", customer, "", "admin", "postgres-search-customer", true});
    ASSERT_TRUE(customer_object) << customer_object.error().message;
    const auto allowed_object = store->create_policy_object({"repository",
                                                             allowed_repository,
                                                             customer_object->id,
                                                             "admin",
                                                             "postgres-search-allowed",
                                                             true});
    ASSERT_TRUE(allowed_object) << allowed_object.error().message;
    const auto denied_object = store->create_policy_object({"repository",
                                                            denied_repository,
                                                            customer_object->id,
                                                            "admin",
                                                            "postgres-search-denied",
                                                            true});
    ASSERT_TRUE(denied_object) << denied_object.error().message;
    const auto grant = store->create_grant({"reader",
                                            allowed_object->id,
                                            "read",
                                            "sha512-256:postgres-search",
                                            "admin",
                                            "postgres-search-grant"});
    ASSERT_TRUE(grant) << grant.error().message;

    TemporarySearchStore search_store;
    lake::search::Service service(search_store.path());
    const std::string allowed_change =
        create_search_change(search_store.path() / customer / "repositories" / allowed_repository,
                             "postgres searchable allowed");
    const std::string denied_change =
        create_search_change(search_store.path() / customer / "repositories" / denied_repository,
                             "postgres searchable denied");
    ASSERT_TRUE(service.publish({customer, allowed_repository, "main", allowed_change}));
    ASSERT_TRUE(service.publish({customer, denied_repository, "main", denied_change}));

    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.search = &service;
    context.authorized = true;
    context.acting_identity = "reader";
    context.request_id = "postgres-search-" + suffix;
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const ApiResult result = registry.call(
        "search", {{"query", "postgres searchable"}, {"customer", customer}}, context);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("mode"), "hot");
    ASSERT_EQ(result.value.at("items").size(), 1U);
    EXPECT_EQ(result.value.at("items").front().at("repository"), allowed_repository);

    context.request_id = "postgres-search-scan-" + suffix;
    const ApiResult scan = registry.call("search",
                                         {{"query", "postgres searchable"},
                                          {"customer", customer},
                                          {"repository", allowed_repository},
                                          {"change", allowed_change}},
                                         context);
    ASSERT_TRUE(scan.ok) << scan.error.message;
    EXPECT_EQ(scan.value.at("mode"), "scan");
    EXPECT_EQ(scan.value.at("scanned_changes"), 1U);
    ASSERT_EQ(scan.value.at("items").size(), 1U);
    EXPECT_EQ(scan.value.at("items").front().at("change"), allowed_change);

    const auto events = store->list_audit_events({.principal = "reader", .limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    const auto scan_event = std::find_if(events->begin(), events->end(), [&](const auto& event) {
        return event.draft.request_id == context.request_id;
    });
    ASSERT_NE(scan_event, events->end());
    EXPECT_EQ(scan_event->draft.action, "search");
    EXPECT_EQ(scan_event->draft.details.value("mode", ""), "scan");
}

TEST(PostgresMetering, AggregatesHourlyUsageAndRecordsBuildOnce) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:meter-" + suffix, "meter-" + suffix + "@example.com", "Meter"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "meter-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string hour = "2026-09-04T10:00:00Z";
    const auto transfer = store->upsert_metering_usage({.customer_id = customer->id,
                                                        .hour = hour,
                                                        .stored_bytes = 4096,
                                                        .transfer_in_bytes = 10,
                                                        .transfer_out_bytes = 20});
    ASSERT_TRUE(transfer) << transfer.error().message;
    EXPECT_EQ(transfer->stored_bytes, 4096);
    const auto first = store->record_metered_build({.customer_id = customer->id,
                                                    .result_id = "result-" + suffix,
                                                    .hour = hour,
                                                    .compute_seconds = 3});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(*first);
    const auto duplicate = store->record_metered_build({.customer_id = customer->id,
                                                        .result_id = "result-" + suffix,
                                                        .hour = hour,
                                                        .compute_seconds = 3});
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_FALSE(*duplicate);
    const auto usage = store->list_metering_usage(customer->id, hour, hour, 10);
    ASSERT_TRUE(usage) << usage.error().message;
    ASSERT_EQ(usage->size(), 1U);
    EXPECT_EQ(usage->front().transfer_in_bytes, 10);
    EXPECT_EQ(usage->front().transfer_out_bytes, 20);
    EXPECT_EQ(usage->front().compute_seconds, 3);
}

TEST(PostgresCapabilities, SecretsAndEncryptionRoundTrip) {
    const char* uri = std::getenv("LAKE_TEST_DB_URI");
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }

    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    ApiContext context;
    context.store = store.get();
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    auto call = [&](const std::string& name, json input = json::object()) {
        context.request_id.clear();
        return registry.call(name, input, context);
    };
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);

    const ApiResult organization =
        call("create_organization", {{"display_name", "Secrets integration " + suffix}});
    ASSERT_TRUE(organization.ok) << organization.error.message;
    const ApiResult customer = call("create_customer",
                                    {{"customer_type", "organization"},
                                     {"organization_id", organization.value.at("id")},
                                     {"name", "secrets-" + suffix}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult mount = call("create_secret_mount",
                                 {{"customer_id", customer.value.at("id")},
                                  {"organization_id", organization.value.at("id")},
                                  {"name", "mount-" + suffix}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value.at("id");
    ASSERT_TRUE(call("get_secret_mount", {{"mount_id", mount_id}}).ok);
    ASSERT_TRUE(call("list_secret_mounts", {{"customer_id", customer.value.at("id")}}).ok);

    const ApiResult first =
        call("put_secret",
             {{"mount_id", mount_id}, {"name", "database"}, {"value", {{"password", "one"}}}});
    ASSERT_TRUE(first.ok) << first.error.message;
    const ApiResult second = call("put_secret",
                                  {{"mount_id", mount_id},
                                   {"name", "database"},
                                   {"value", {{"password", "two"}}},
                                   {"check_and_set", 1}});
    ASSERT_TRUE(second.ok) << second.error.message;
    const ApiResult metadata =
        call("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(metadata.ok) << metadata.error.message;
    EXPECT_EQ(metadata.value.at("versions").size(), 2);
    ASSERT_TRUE(call("delete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    EXPECT_FALSE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);
    ASSERT_TRUE(call("undelete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    ASSERT_TRUE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);
    ASSERT_TRUE(call("destroy_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    EXPECT_FALSE(
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}}).ok);

    const ApiResult key =
        call("create_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}});
    ASSERT_TRUE(key.ok) << key.error.message;
    ASSERT_TRUE(call("get_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}}).ok);
    ASSERT_TRUE(call("list_encryption_keys", {{"mount_id", mount_id}}).ok);
    const std::string payload = "cG9zdGdyZXM";
    const ApiResult encrypted =
        call("encrypt", {{"mount_id", mount_id}, {"name", "transit"}, {"plaintext", payload}});
    ASSERT_TRUE(encrypted.ok) << encrypted.error.message;
    ASSERT_TRUE(call("rotate_encryption_key", {{"mount_id", mount_id}, {"name", "transit"}}).ok);
    const ApiResult decrypted = call(
        "decrypt",
        {{"mount_id", mount_id}, {"name", "transit"}, {"ciphertext", encrypted.value.at("value")}});
    ASSERT_TRUE(decrypted.ok) << decrypted.error.message;
    EXPECT_EQ(decrypted.value.at("value"), payload);
    const ApiResult rewrapped = call(
        "rewrap",
        {{"mount_id", mount_id}, {"name", "transit"}, {"ciphertext", encrypted.value.at("value")}});
    ASSERT_TRUE(rewrapped.ok) << rewrapped.error.message;
    EXPECT_EQ(rewrapped.value.at("key_version"), 2);
    const ApiResult signature =
        call("sign", {{"mount_id", mount_id}, {"name", "transit"}, {"message", payload}});
    ASSERT_TRUE(signature.ok) << signature.error.message;
    const ApiResult verified = call("verify",
                                    {{"mount_id", mount_id},
                                     {"name", "transit"},
                                     {"message", payload},
                                     {"signature", signature.value.at("value")}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(verified.value.at("valid"));
    ASSERT_TRUE(
        call("hmac", {{"mount_id", mount_id}, {"name", "transit"}, {"message", payload}}).ok);
    ASSERT_TRUE(call("derive_encryption_key",
                     {{"mount_id", mount_id},
                      {"parent_name", "transit"},
                      {"name", "derived"},
                      {"context", "Y29udGV4dA"}})
                    .ok);

    const ApiResult kek = call("rotate_customer_kek", {{"customer_id", customer.value.at("id")}});
    ASSERT_TRUE(kek.ok) << kek.error.message;
    EXPECT_GE(kek.value.at("data_keys_rewrapped"), 4);
    const ApiResult after = call("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value.at("value").at("password"), "two");
    ASSERT_TRUE(call("decrypt",
                     {{"mount_id", mount_id},
                      {"name", "transit"},
                      {"ciphertext", rewrapped.value.at("value")}})
                    .ok);

    context.operator_credential = false;
    context.acting_identity = "taap:1:ed25519:no-mount-grant-" + suffix;
    const ApiResult denied = call("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.outcome, "denied");
    EXPECT_EQ(event->draft.details.at("mount_id"), mount_id);
}

} // namespace
