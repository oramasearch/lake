#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <liquid/liquid.hpp>
#include <pqxx/pqxx>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/agent_runs.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/platform_store.hpp"
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
    const char* uri = lake::test::postgres_uri();
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
        context.request_id = lake::control::new_uuid_v4();
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
    const std::string deletion_request = context.request_id;
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_FALSE(deleted.value.at("deleted_at").is_null());
    EXPECT_EQ(
        call("rename_repository", {{"id", repository.value.at("id")}, {"name", "again-" + suffix}})
            .error.code,
        "conflict");
    const ApiResult audit =
        call("audit",
             {{"object", "repository/" + repository.value.at("id").get<std::string>()},
              {"outcome", "success"},
              {"request_id", deletion_request},
              {"limit", 1}});
    ASSERT_TRUE(audit.ok) << audit.error.message;
    ASSERT_EQ(audit.value.at("items").size(), 1U);
    EXPECT_EQ(audit.value.at("items").at(0).at("action"), "delete_repository");
    EXPECT_EQ(audit.value.at("items").at(0).at("outcome"), "success");

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
    const char* uri = lake::test::postgres_uri();
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
    EXPECT_EQ(link->expires_at, "2999-01-01T00:00:00.000000Z");
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
    const char* uri = lake::test::postgres_uri();
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

    const auto events = lake::test::audit_events(*store, {.request_id = context.request_id});
    ASSERT_TRUE(events) << events.error().message;
    const auto scan_event = std::find_if(events->begin(), events->end(), [&](const auto& event) {
        return event.draft.request_id == context.request_id;
    });
    ASSERT_NE(scan_event, events->end());
    EXPECT_EQ(scan_event->draft.action, "search");
    EXPECT_EQ(scan_event->draft.details.value("mode", ""), "scan");
}

TEST(PostgresMetering, AggregatesHourlyUsageAndRecordsBuildOnce) {
    const char* uri = lake::test::postgres_uri();
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
    const char* uri = lake::test::postgres_uri();
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

TEST(PostgresPlatform, EnvironmentBindingAndMirrorRoundTrip) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    auto* platform_store = dynamic_cast<lake::control::PlatformStore*>(store.get());
    ASSERT_NE(platform_store, nullptr);
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:platform-" + suffix, "platform-" + suffix + "@example.test", "Platform"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        store->create_customer({"individual", user->id, "", "platform-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        store->create_repository({customer->id, "platform-" + suffix, "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto environment =
        platform_store->ensure_platform_environment(repository->id, "production");
    ASSERT_TRUE(environment) << environment.error().message;
    const auto same = platform_store->ensure_platform_environment(repository->id, "production");
    ASSERT_TRUE(same) << same.error().message;
    EXPECT_EQ(same->id, environment->id);
    const auto object = store->get_policy_object(environment->object_id);
    ASSERT_TRUE(object) << object.error().message;
    EXPECT_EQ(object->object_type, "environment");
    ASSERT_TRUE(platform_store->upsert_platform_mirror(
        {"deployment",
         "deployment-" + suffix,
         {{"repository", repository->id}, {"environment", "production"}, {"phase", "ready"}},
         "2026-09-06T00:00:00Z"}));
    const auto rows = platform_store->list_platform_mirror("deployment");
    ASSERT_TRUE(rows) << rows.error().message;
    const auto found =
        std::ranges::find(*rows, "deployment-" + suffix, &lake::control::PlatformMirrorRow::key);
    ASSERT_NE(found, rows->end());
    EXPECT_EQ(found->value.at("phase"), "ready");
}

} // namespace

TEST(PostgresServices, CreateListAndWithdrawFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:svc-" + suffix, "svc-" + suffix + "@example.com", "Services"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "svc-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    lake::control::NewManagedService workspace{customer->id, "zetta", "workspace", "taap:x"};
    workspace.is_default = true;
    const auto first = store->create_service(workspace);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->slug, lake::control::managed_service_slug("ws", customer->id));
    EXPECT_EQ(first->instances, 1);
    EXPECT_EQ(first->storage_gi, 10);
    EXPECT_EQ(first->phase, "") << "no status row yet";
    const auto again = store->create_service(workspace);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->id, first->id) << "the default is created once";

    lake::control::NewManagedService postgres{customer->id, "postgres", "crm", "taap:x"};
    postgres.instances = 2;
    postgres.storage_gi = 30;
    postgres.max_per_customer = 2;
    const auto crm = store->create_service(postgres);
    ASSERT_TRUE(crm) << crm.error().message;
    EXPECT_TRUE(crm->slug.starts_with("svc-"));
    EXPECT_EQ(crm->instances, 2);
    EXPECT_EQ(crm->storage_gi, 30);
    EXPECT_EQ(store->create_service(postgres).error().code, "conflict");
    postgres.name = "other";
    EXPECT_EQ(store->create_service(postgres).error().code, "quota_exceeded");

    const auto withdrawn = store->withdraw_service(first->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_EQ(withdrawn->generation, 2);
    EXPECT_EQ(store->withdraw_service(first->id).error().code, "conflict");
    const auto second = store->create_service(workspace);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second->id, first->id);
    EXPECT_TRUE(second->slug.starts_with("svc-")) << "the ws- slug is never reused";

    const auto listed = store->list_services(customer->id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 3U)
        << "a withdrawn service stays listed until the operator deletes it";
    EXPECT_EQ(store->get_service(lake::control::new_uuid_v4()).error().code, "not_found");
}

TEST(PostgresStores, CreateListWithdrawAndNotifyFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ASSERT_TRUE(store->ping()) << store->ping().error().message;
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:st-" + suffix, "st-" + suffix + "@example.com", "Stores"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "st-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;

    // The operator's listener: every insert and withdrawal wakes it on the services channel.
    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_services",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    lake::control::NewManagedStore volume{customer->id, "volume", "cluster", "uploads", "taap:x"};
    volume.size_gi = 20;
    const auto first = store->create_store(volume);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->slug, lake::control::managed_service_slug("st", first->id));
    EXPECT_EQ(first->type, "volume");
    EXPECT_EQ(first->provider, "cluster");
    EXPECT_EQ(first->size_gi, 20);
    EXPECT_FALSE(first->is_public);
    EXPECT_EQ(first->desired, "present");
    EXPECT_EQ(first->generation, 1);
    EXPECT_EQ(first->phase, "") << "no status row yet";
    EXPECT_EQ(store->create_store(volume).error().code, "conflict");
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        const auto spec =
            tx.exec_params1("SELECT spec::text FROM stores WHERE id = $1::uuid", first->id)[0]
                .as<std::string>();
        const json parsed = json::parse(spec);
        EXPECT_EQ(parsed["size_gi"], 20);
        EXPECT_EQ(parsed["public"], false);
        EXPECT_TRUE(parsed.contains("region"));
    }

    const auto withdrawn = store->withdraw_store(first->id);
    ASSERT_TRUE(withdrawn) << withdrawn.error().message;
    EXPECT_EQ(withdrawn->desired, "absent");
    EXPECT_EQ(withdrawn->generation, 2);
    EXPECT_EQ(store->withdraw_store(first->id).error().code, "conflict");
    const auto second = store->create_store(volume);
    ASSERT_TRUE(second) << "a withdrawn store frees its name: " << second.error().message;
    EXPECT_NE(second->slug, first->slug) << "a slug is never reused";

    for (int waited = 0; notified.size() < 3 && waited < 50; ++waited) {
        listener.await_notification(0, 100000);
    }
    EXPECT_EQ(notified, (std::vector<std::string>{first->id, first->id, second->id}));

    auto listed = store->list_stores(customer->id);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->size(), 2U) << "a withdrawn store stays listed until the operator deletes it";
    EXPECT_EQ(listed->front().id, first->id) << "oldest first";
    {
        // What the operator writes once the claim is gone.
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO store_status (store_id, phase, message, observed_generation) "
                       "VALUES ($1::uuid, 'Deleted', 'deleted', 2)",
                       first->id);
        tx.exec_params("INSERT INTO store_status (store_id, phase, message, endpoint, "
                       "observed_generation) VALUES ($1::uuid, 'Ready', 'binds on first use', "
                       "'pvc/x', 1)",
                       second->id);
        tx.commit();
    }
    listed = store->list_stores(customer->id);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U) << "a finished deletion is history";
    EXPECT_EQ(listed->front().phase, "Ready");
    EXPECT_EQ(listed->front().message, "binds on first use");
    EXPECT_EQ(listed->front().endpoint, "pvc/x");
    EXPECT_EQ(listed->front().observed_generation, 1);
    EXPECT_EQ(store->get_store(first->id)->phase, "Deleted") << "history is still readable";
    EXPECT_EQ(store->get_store(lake::control::new_uuid_v4()).error().code, "not_found");
}

// Agent launches, their credential, cron schedules and Ask Orama messages against the production store
// (docs/api/agent-launches.md, lake#231).
TEST(PostgresAgentLaunches, LaunchesCredentialsSchedulesAndConversationsFollowTheContract) {
    const char* uri = lake::test::postgres_uri();
    if (uri == nullptr || std::string(uri).empty()) {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not set";
    }
    auto store = lake::control::make_postgres_store(
        uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
    const auto user = store->create_user(
        {"taap:1:ed25519:al-" + suffix, "al-" + suffix + "@example.com", "Launcher"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = store->create_customer({"individual", user->id, "", "al-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    pqxx::connection listener(uri);
    std::vector<std::string> notified;
    listener.listen("lake_services",
                    [&](pqxx::notification note) { notified.emplace_back(note.payload); });

    const auto launch = store->create_agent_launch({customer->id,
                                                    "a-1",
                                                    "Watcher",
                                                    "service",
                                                    "manual",
                                                    json{{"week", 39}},
                                                    lake::control::agent_launch_scopes,
                                                    user->id,
                                                    user->taap_id});
    ASSERT_TRUE(launch) << launch.error().message;
    EXPECT_EQ(launch->slug, lake::control::managed_service_slug("al", launch->id));
    EXPECT_EQ(launch->input["week"], 39);
    EXPECT_EQ(launch->scopes, lake::control::agent_launch_scopes);
    EXPECT_EQ(launch->phase, "") << "no status row yet";
    listener.await_notification(5, 0);
    EXPECT_NE(std::ranges::find(notified, launch->id), notified.end());

    const std::string credential = "lal." + launch->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store->context_for(credential)) << "not before the operator reports its digest";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_launch_status (launch_id, phase, endpoint, "
                       "credential_sha256, exit_code, result) VALUES ($1::uuid, 'Ready', "
                       "'http://al:8080', $2, NULL, '{\"reply\":\"ok\"}'::jsonb)",
                       launch->id,
                       lake::control::identity_crypto::sha256_hex(credential));
        tx.commit();
    }
    const auto acting = store->context_for(credential);
    ASSERT_TRUE(acting) << acting.error().message;
    EXPECT_EQ(acting->user_id, user->id);
    EXPECT_EQ(acting->kind, "agent_launch");
    EXPECT_EQ(acting->scopes, lake::control::agent_launch_scopes);
    const auto listed = store->list_agent_launches(customer->id, "a-1", 10);
    ASSERT_TRUE(listed && listed->size() == 1U);
    EXPECT_EQ(listed->front().phase, "Ready");
    EXPECT_EQ(listed->front().endpoint, "http://al:8080");
    EXPECT_EQ(listed->front().result["reply"], "ok");
    ASSERT_TRUE(store->withdraw_agent_launch(launch->id));
    EXPECT_FALSE(store->context_for(credential)) << "a stopped launch's credential stops too";

    // Cron schedules: several per agent, each with its own slug and credential (migration 0031).
    const auto weekly = store->create_agent_cron_schedule({.customer_id = customer->id,
                                                           .agent_id = "a-1",
                                                           .agent_name = "Watcher",
                                                           .cron = "0 9 * * 1",
                                                           .description = "every monday at 09:00",
                                                           .input = json{{"week", 39}},
                                                           .scopes =
                                                               lake::control::agent_launch_scopes,
                                                           .created_by_user_id = user->id,
                                                           .created_by_taap_id = user->taap_id});
    ASSERT_TRUE(weekly) << weekly.error().message;
    const auto nightly = store->create_agent_cron_schedule({.customer_id = customer->id,
                                                            .agent_id = "a-1",
                                                            .agent_name = "Watcher",
                                                            .cron = "30 2 * * *",
                                                            .time_zone = "America/Toronto",
                                                            .scopes =
                                                                lake::control::agent_launch_scopes,
                                                            .created_by_user_id = user->id,
                                                            .created_by_taap_id = user->taap_id});
    ASSERT_TRUE(nightly) << nightly.error().message;
    EXPECT_EQ(weekly->slug, lake::control::managed_service_slug("as", weekly->id));
    EXPECT_EQ(weekly->time_zone, "Etc/UTC");
    EXPECT_EQ(nightly->time_zone, "America/Toronto");
    listener.await_notification(5, 0);
    EXPECT_NE(std::ranges::find(notified, weekly->id), notified.end());
    auto schedules = store->list_agent_cron_schedules(customer->id, "a-1");
    ASSERT_TRUE(schedules && schedules->size() == 2U);
    EXPECT_EQ(schedules->front().id, weekly->id) << "oldest first";

    const std::string schedule_credential =
        "las." + weekly->id + ".0123456789abcdef0123456789abcdef";
    EXPECT_FALSE(store->context_for(schedule_credential));
    const std::string run_id = lake::control::new_uuid_v4();
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO agent_cron_schedule_status (schedule_id, phase, message, "
                       "last_schedule_at, credential_sha256, observed_generation) VALUES "
                       "($1::uuid, 'Ready', 'scheduled', now(), $2, 1)",
                       weekly->id,
                       lake::control::identity_crypto::sha256_hex(schedule_credential));
        tx.exec_params("INSERT INTO agent_cron_runs (id, schedule_id, job_name, phase, message, "
                       "exit_code, result) VALUES ($1::uuid, $2::uuid, 'lake-as-x-1', "
                       "'Succeeded', 'finished', 0, '{\"reply\":\"3 at risk\"}'::jsonb)",
                       run_id,
                       weekly->id);
        tx.commit();
    }
    const auto scheduled_acting = store->context_for(schedule_credential);
    ASSERT_TRUE(scheduled_acting) << scheduled_acting.error().message;
    EXPECT_EQ(scheduled_acting->user_id, user->id);
    EXPECT_EQ(scheduled_acting->kind, "agent_schedule");
    const auto got = store->get_agent_cron_schedule(weekly->id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->phase, "Ready");
    EXPECT_FALSE(got->last_schedule_at.empty());
    const auto runs = store->list_agent_cron_runs(customer->id, "a-1", 10);
    ASSERT_TRUE(runs && runs->size() == 1U);
    EXPECT_EQ(runs->front().id, run_id);
    EXPECT_EQ(runs->front().trigger, "schedule");
    EXPECT_EQ(runs->front().exit_code, 0);
    EXPECT_EQ(runs->front().result["reply"], "3 at risk");
    EXPECT_EQ(runs->front().input["week"], 39);
    ASSERT_TRUE(store->withdraw_agent_cron_schedule(weekly->id));
    EXPECT_FALSE(store->context_for(schedule_credential)) << "a removed schedule's credential stops";
    EXPECT_FALSE(store->withdraw_agent_cron_schedule(weekly->id)) << "already removed";
    schedules = store->list_agent_cron_schedules(customer->id, "a-1");
    ASSERT_TRUE(schedules && schedules->size() == 1U);

    const std::string conversation = lake::control::new_uuid_v4();
    ASSERT_TRUE(store->append_ask_message(
        {"", conversation, customer->id, user->id, "user", "what runs?", nullptr, ""}));
    ASSERT_TRUE(store->append_ask_message({"",
                                           conversation,
                                           customer->id,
                                           user->id,
                                           "assistant",
                                           "two runs",
                                           json{{"reads", json::array()}},
                                           ""}));
    const auto messages = store->ask_conversation(conversation, user->id, 50);
    ASSERT_TRUE(messages && messages->size() == 2U);
    EXPECT_EQ(messages->at(0).role, "user");
    EXPECT_EQ(messages->at(1).payload["reads"], json::array());
    EXPECT_TRUE(store->ask_conversation(conversation, lake::control::new_uuid_v4(), 50)->empty())
        << "only the person who had the conversation reads it back";
}
