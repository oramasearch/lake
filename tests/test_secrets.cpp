// Static secrets and named encryption keys (docs/api/secrets.md), kept in the workspace Zetta of
// their mount (lake#328-#332): version history, delete/undelete/destroy, transit operations, the
// customer KEK rotation across workspaces, and the conditional writes that keep versions unique.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/secret_records_api.hpp"
#include "api/workspace_records_api.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/secret_records.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

constexpr std::string_view kek = "7365637265747320696e20776f726b7370616365732074657374206b656b2121";

class SecretsCapabilities : public ::testing::Test {
protected:
    void SetUp() override {
        lake::api::register_core_capabilities(registry, "test");
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = std::string(kek);

        const ApiResult organization =
            call("create_organization", {{"display_name", "Secrets organization"}});
        ASSERT_TRUE(organization.ok) << organization.error.message;
        organization_id = organization.value.at("id");
        const ApiResult customer = call("create_customer",
                                        {{"customer_type", "organization"},
                                         {"organization_id", organization_id},
                                         {"name", "secrets-customer"}});
        ASSERT_TRUE(customer.ok) << customer.error.message;
        customer_id = customer.value.at("id");
        workspace_id = workspaces.create(customer_id, "secrets");
        const ApiResult mount = call("create_secret_mount",
                                     {{"customer_id", customer_id},
                                      {"workspace_id", workspace_id},
                                      {"organization_id", organization_id},
                                      {"name", "application"},
                                      {"description", "Application credentials"}});
        ASSERT_TRUE(mount.ok) << mount.error.message;
        mount_id = mount.value.at("id");
    }

    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id.clear();
        const auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    }

    Registry registry;
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    ApiContext context;
    std::string organization_id;
    std::string customer_id;
    std::string workspace_id;
    std::string mount_id;
};

TEST_F(SecretsCapabilities, VersionHistoryDeleteUndeleteAndDestroy) {
    const ApiResult got = call("get_secret_mount", {{"mount_id", mount_id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("workspace_id"), workspace_id);
    const ApiResult mounts = call("list_secret_mounts",
                                  {{"customer_id", customer_id}, {"workspace_id", workspace_id}});
    ASSERT_TRUE(mounts.ok) << mounts.error.message;
    ASSERT_EQ(mounts.value.at("items").size(), 1);

    const ApiResult first = call("put_secret",
                                 {{"mount_id", mount_id},
                                  {"name", "database"},
                                  {"value", {{"password", "first"}}},
                                  {"custom_metadata", {{"owner", "payments"}}}});
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value.at("version"), 1);
    // put_secret answers metadata only; read_secret is the one call with a value (lake#590).
    EXPECT_FALSE(first.value.contains("value")) << first.value.dump();
    EXPECT_EQ(first.value.dump().find("first"), std::string::npos) << first.value.dump();
    EXPECT_EQ(first.value.at("name"), "database");
    const ApiResult second = call("put_secret",
                                  {{"mount_id", mount_id},
                                   {"name", "database"},
                                   {"value", {{"password", "second"}}},
                                   {"check_and_set", 1}});
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("version"), 2);
    EXPECT_EQ(second.value.at("custom_metadata").at("owner"), "payments")
        << "omitted metadata keeps the entry's";
    const ApiResult stale = call("put_secret",
                                 {{"mount_id", mount_id},
                                  {"name", "database"},
                                  {"value", {{"password", "stale"}}},
                                  {"check_and_set", 1}});
    EXPECT_EQ(stale.error.code, "conflict");

    const ApiResult history =
        call("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(history.ok) << history.error.message;
    EXPECT_EQ(history.value.at("current_version"), 2);
    EXPECT_EQ(history.value.at("versions").size(), 2);

    ASSERT_TRUE(call("delete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    const ApiResult deleted =
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}});
    EXPECT_FALSE(deleted.ok);
    EXPECT_EQ(deleted.error.code, "not_found");
    EXPECT_EQ(deleted.error.path, "/version");
    // Every refusal names the field and says why (lake#592).
    const ApiResult missing = call("read_secret", {{"mount_id", mount_id}, {"name", "nope"}});
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_EQ(missing.error.message, "no secret named nope");
    EXPECT_EQ(missing.error.path, "/name");
    const ApiResult no_version =
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 9}});
    EXPECT_EQ(no_version.error.message, "secret database has no version 9");
    EXPECT_EQ(no_version.error.path, "/version");
    const ApiResult no_history = call("secret_metadata", {{"mount_id", mount_id}, {"name", "nope"}});
    EXPECT_EQ(no_history.error.path, "/name");
    ASSERT_TRUE(call("undelete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    const ApiResult restored =
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}});
    ASSERT_TRUE(restored.ok) << restored.error.message;
    EXPECT_EQ(restored.value.at("value").at("password"), "first");

    ASSERT_TRUE(call("destroy_secret_versions",
                     {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}})
                    .ok);
    const ApiResult destroyed =
        call("read_secret", {{"mount_id", mount_id}, {"name", "database"}, {"version", 1}});
    EXPECT_FALSE(destroyed.ok);
    EXPECT_EQ(destroyed.error.code, "not_found");
    const ApiResult undestroy = call(
        "undelete_secret_versions", {{"mount_id", mount_id}, {"name", "database"}, {"versions", {1}}});
    EXPECT_EQ(undestroy.error.code, "conflict");
    const ApiResult destroyed_history =
        call("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(destroyed_history.ok) << destroyed_history.error.message;
    EXPECT_FALSE(destroyed_history.value.at("versions")
                     .at(0)
                     .at("destroyed_at")
                     .get<std::string>()
                     .empty());
    for (const auto& [key, row] : workspaces.records(workspace_id).rows("secrets_versions")) {
        EXPECT_EQ(row.dump().find("first"), std::string::npos) << "values are sealed";
    }
    const ApiResult third = call(
        "put_secret",
        {{"mount_id", mount_id}, {"name", "database"}, {"value", {{"password", "third"}}}});
    ASSERT_TRUE(third.ok) << third.error.message;
    EXPECT_EQ(third.value.at("version"), 3) << "a number is never reused";
}

TEST_F(SecretsCapabilities, ListSecretsAnswersNamesAndStateNeverValues) {
    const auto put = [this](const std::string& name, const std::string& password) {
        const ApiResult result =
            call("put_secret",
                 {{"mount_id", mount_id}, {"name", name}, {"value", {{"password", password}}}});
        ASSERT_TRUE(result.ok) << result.error.message;
    };
    put("database", "database-one");
    put("database", "database-two");
    put("api/token", "token-one");
    put("api/key", "key-one");
    ASSERT_TRUE(call("delete_secret_versions",
                     {{"mount_id", mount_id}, {"name", "api/key"}, {"versions", {1}}})
                    .ok);

    const ApiResult all = call("list_secrets", {{"mount_id", mount_id}});
    ASSERT_TRUE(all.ok) << all.error.message;
    const json& items = all.value.at("items");
    ASSERT_EQ(items.size(), 3u);
    EXPECT_FALSE(all.value.contains("next_cursor"));
    EXPECT_EQ(items.at(0).at("name"), "api/key") << "names are in order";
    EXPECT_EQ(items.at(1).at("name"), "api/token");
    EXPECT_EQ(items.at(2).at("name"), "database");
    EXPECT_EQ(items.at(2).at("current_version"), 2);
    EXPECT_FALSE(items.at(2).at("created_at").get<std::string>().empty());
    EXPECT_FALSE(items.at(2).at("updated_at").get<std::string>().empty());
    EXPECT_FALSE(items.at(2).at("deleted"));
    EXPECT_TRUE(items.at(2).at("deleted_at").is_null());
    EXPECT_TRUE(items.at(0).at("deleted")) << "the current version is deleted";
    EXPECT_FALSE(items.at(0).at("deleted_at").is_null());
    EXPECT_TRUE(items.at(0).at("destroyed_at").is_null());
    const std::string dumped = all.value.dump();
    for (const char* value : {"database-one", "database-two", "token-one", "key-one"}) {
        EXPECT_EQ(dumped.find(value), std::string::npos) << "no value is listed: " << value;
    }
    for (const auto& item : items) {
        EXPECT_FALSE(item.contains("value"));
    }

    const ApiResult prefixed = call("list_secrets", {{"mount_id", mount_id}, {"prefix", "api/"}});
    ASSERT_TRUE(prefixed.ok) << prefixed.error.message;
    ASSERT_EQ(prefixed.value.at("items").size(), 2u);

    // One at a time: the cursor walks every name once.
    std::vector<std::string> walked;
    json input = {{"mount_id", mount_id}, {"limit", 1}};
    for (int page = 0; page < 5; ++page) {
        const ApiResult got = call("list_secrets", input);
        ASSERT_TRUE(got.ok) << got.error.message;
        for (const auto& item : got.value.at("items")) {
            walked.push_back(item.at("name"));
        }
        if (!got.value.contains("next_cursor")) {
            break;
        }
        input["cursor"] = got.value.at("next_cursor");
    }
    EXPECT_EQ(walked, (std::vector<std::string>{"api/key", "api/token", "database"}));

    const ApiResult destroyed =
        call("destroy_secret_versions",
             {{"mount_id", mount_id}, {"name", "api/key"}, {"versions", {1}}});
    ASSERT_TRUE(destroyed.ok) << destroyed.error.message;
    const ApiResult after = call("list_secrets", {{"mount_id", mount_id}, {"prefix", "api/key"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    ASSERT_EQ(after.value.at("items").size(), 1u);
    EXPECT_TRUE(after.value.at("items").at(0).at("deleted"));
    EXPECT_FALSE(after.value.at("items").at(0).at("destroyed_at").is_null());

    const ApiResult unknown =
        call("list_secrets", {{"mount_id", "00000000-0000-4000-8000-000000000000"}});
    EXPECT_EQ(unknown.error.code, "not_found");
    EXPECT_EQ(unknown.error.path, "/mount_id");
    const ApiResult too_many = call("list_secrets", {{"mount_id", mount_id}, {"limit", 501}});
    EXPECT_EQ(too_many.error.code, "invalid_input");

    EXPECT_EQ(registry.find("list_secrets")->kind, lake::api::CapabilityKind::Read);
    EXPECT_TRUE(registry.find("list_secrets")->authorizes_candidates);
    EXPECT_EQ(lake::api::audit_log_of("list_secrets"), lake::api::AuditLog::Workspace);

    // A caller with no grant on the mount is refused, audited, and reaches no workspace.
    context.operator_credential = false;
    context.acting_identity = "taap:1:ed25519:ungranted";
    const auto calls = workspaces.records(workspace_id).calls.size();
    const ApiResult denied = call("list_secrets", {{"mount_id", mount_id}});
    EXPECT_EQ(denied.error.code, "unauthorized");
    EXPECT_EQ(workspaces.records(workspace_id).calls.size(), calls);
    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.outcome, "denied");
    EXPECT_EQ(event->draft.object, "secret_mount/" + mount_id);
}

TEST_F(SecretsCapabilities, APutThatStoppedBeforeMovingTheEntryIsCompletedByTheNext) {
    ASSERT_TRUE(call("put_secret",
                     {{"mount_id", mount_id}, {"name", "token"}, {"value", {{"v", "one"}}}})
                    .ok);
    // A writer claimed version 2 and stopped before moving the entry: its record exists, the
    // entry still says 1.
    auto secrets = lake::api::MountSecrets::open(context, mount_id);
    ASSERT_TRUE(secrets) << secrets.error().message;
    lake::control::SecretVersionRecord orphan;
    orphan.mount_id = mount_id;
    orphan.name = "token";
    orphan.version = 2;
    orphan.ciphertext = "c2VhbGVk";
    orphan.metadata = json{{"by", "stopped writer"}};
    auto& records = secrets->records();
    ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(records.put(
        lake::control::SecretVersionCodec::collection,
        lake::control::SecretVersionCodec::key(records, mount_id, "token", 2),
        lake::control::SecretVersionCodec::lookup(mount_id, "token"),
        lake::control::SecretVersionCodec::encode(orphan),
        lake::control::RecordCondition{.if_absent = true}))));
    // A conditional put against version 1 is refused, and moves the entry to 2 as the stopped put
    // would have, so the caller rereads and succeeds.
    const ApiResult refused = call("put_secret",
                                   {{"mount_id", mount_id},
                                    {"name", "token"},
                                    {"value", {{"v", "two"}}},
                                    {"check_and_set", 1}});
    EXPECT_EQ(refused.error.code, "conflict");
    const ApiResult moved = call("secret_metadata", {{"mount_id", mount_id}, {"name", "token"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(moved.value.at("current_version"), 2);
    EXPECT_EQ(moved.value.at("custom_metadata").at("by"), "stopped writer");
    const ApiResult next = call("put_secret",
                                {{"mount_id", mount_id},
                                 {"name", "token"},
                                 {"value", {{"v", "three"}}},
                                 {"check_and_set", 2}});
    ASSERT_TRUE(next.ok) << next.error.message;
    EXPECT_EQ(next.value.at("version"), 3);
    const ApiResult read = call("read_secret", {{"mount_id", mount_id}, {"name", "token"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value.at("value").at("v"), "three");
}

TEST_F(SecretsCapabilities, RotationRewrapSigningHmacDerivationAndKekRotation) {
    const ApiResult created =
        call("create_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(
        call("create_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}}).error.code,
        "conflict");
    ASSERT_TRUE(call("get_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}}).ok);
    const ApiResult keys = call("list_encryption_keys", {{"mount_id", mount_id}});
    ASSERT_TRUE(keys.ok) << keys.error.message;
    ASSERT_EQ(keys.value.at("items").size(), 1);

    const std::string payload = "cGF5bG9hZA";
    const ApiResult encrypted =
        call("encrypt", {{"mount_id", mount_id}, {"name", "orders"}, {"plaintext", payload}});
    ASSERT_TRUE(encrypted.ok) << encrypted.error.message;
    EXPECT_EQ(encrypted.value.at("key_version"), 1);
    const std::string first_ciphertext = encrypted.value.at("value");

    const ApiResult rotated =
        call("rotate_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}});
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(rotated.value.at("current_version"), 2);
    const ApiResult new_encrypted =
        call("encrypt", {{"mount_id", mount_id}, {"name", "orders"}, {"plaintext", payload}});
    ASSERT_TRUE(new_encrypted.ok) << new_encrypted.error.message;
    EXPECT_EQ(new_encrypted.value.at("key_version"), 2);
    const ApiResult old_decrypted = call(
        "decrypt", {{"mount_id", mount_id}, {"name", "orders"}, {"ciphertext", first_ciphertext}});
    ASSERT_TRUE(old_decrypted.ok) << old_decrypted.error.message;
    EXPECT_EQ(old_decrypted.value.at("value"), payload);
    EXPECT_EQ(old_decrypted.value.at("key_version"), 1);
    const ApiResult rewrapped = call(
        "rewrap", {{"mount_id", mount_id}, {"name", "orders"}, {"ciphertext", first_ciphertext}});
    ASSERT_TRUE(rewrapped.ok) << rewrapped.error.message;
    EXPECT_EQ(rewrapped.value.at("key_version"), 2);
    EXPECT_NE(rewrapped.value.at("value"), first_ciphertext);

    const ApiResult signature =
        call("sign", {{"mount_id", mount_id}, {"name", "orders"}, {"message", payload}});
    ASSERT_TRUE(signature.ok) << signature.error.message;
    const ApiResult verified = call("verify",
                                    {{"mount_id", mount_id},
                                     {"name", "orders"},
                                     {"message", payload},
                                     {"signature", signature.value.at("value")}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(verified.value.at("valid"));
    const ApiResult digest = call(
        "hmac", {{"mount_id", mount_id}, {"name", "orders"}, {"message", payload}, {"version", 1}});
    ASSERT_TRUE(digest.ok) << digest.error.message;
    EXPECT_EQ(digest.value.at("algorithm"), "hmac-sha256");
    const ApiResult derived = call("derive_encryption_key",
                                   {{"mount_id", mount_id},
                                    {"parent_name", "orders"},
                                    {"name", "orders-region"},
                                    {"context", "cmVnaW9u"}});
    ASSERT_TRUE(derived.ok) << derived.error.message;
    EXPECT_EQ(derived.value.at("derived_from"), created.value.at("id"));
    EXPECT_FALSE(derived.value.contains("value"));

    ASSERT_TRUE(
        call("put_secret",
             {{"mount_id", mount_id}, {"name", "stable"}, {"value", {{"token", "unchanged"}}}})
            .ok);
    const ApiResult kek_rotation = call("rotate_customer_kek", {{"customer_id", customer_id}});
    ASSERT_TRUE(kek_rotation.ok) << kek_rotation.error.message;
    EXPECT_EQ(kek_rotation.value.at("version"), 2);
    // The mount's data key and the named keys' three versions, all in the workspace.
    EXPECT_GE(kek_rotation.value.at("data_keys_rewrapped"), 4);
    const auto state = store->customer_kek_state(customer_id);
    ASSERT_TRUE(state);
    EXPECT_EQ(state->previous_version, 0U) << "the replaced KEK is forgotten once rewrapped";
    const ApiResult after = call("read_secret", {{"mount_id", mount_id}, {"name", "stable"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value.at("value").at("token"), "unchanged");
    const ApiResult rewrapped_decrypted = call(
        "decrypt",
        {{"mount_id", mount_id}, {"name", "orders"}, {"ciphertext", rewrapped.value.at("value")}});
    ASSERT_TRUE(rewrapped_decrypted.ok) << rewrapped_decrypted.error.message;
    EXPECT_EQ(rewrapped_decrypted.value.at("value"), payload);
}

TEST_F(SecretsCapabilities, AKekRotationAWorkspaceCouldNotFinishIsFinishedByTheNext) {
    const std::string second_workspace = workspaces.create(customer_id, "secrets-two");
    const ApiResult second_mount = call("create_secret_mount",
                                        {{"customer_id", customer_id},
                                         {"workspace_id", second_workspace},
                                         {"organization_id", organization_id},
                                         {"name", "application"}});
    ASSERT_TRUE(second_mount.ok) << second_mount.error.message;
    const std::string second_mount_id = second_mount.value.at("id");
    ASSERT_TRUE(
        call("put_secret", {{"mount_id", mount_id}, {"name", "a"}, {"value", {{"v", "first"}}}})
            .ok);
    ASSERT_TRUE(call("put_secret",
                     {{"mount_id", second_mount_id}, {"name", "b"}, {"value", {{"v", "second"}}}})
                    .ok);
    workspaces.status(second_workspace, "Provisioning");
    const ApiResult stopped = call("rotate_customer_kek", {{"customer_id", customer_id}});
    EXPECT_FALSE(stopped.ok);
    EXPECT_EQ(stopped.error.code, "workspace_not_ready");
    const auto pending = store->customer_kek_state(customer_id);
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->version, 2U);
    EXPECT_EQ(pending->previous_version, 1U) << "kept while a workspace still uses it";
    // The ready workspace was rewrapped and reads on.
    const ApiResult first = call("read_secret", {{"mount_id", mount_id}, {"name", "a"}});
    ASSERT_TRUE(first.ok) << first.error.message;

    workspaces.status(second_workspace, "Ready");
    // Still readable under the previous KEK before the rotation is finished.
    const ApiResult before = call("read_secret", {{"mount_id", second_mount_id}, {"name", "b"}});
    ASSERT_TRUE(before.ok) << before.error.message;
    const ApiResult finished = call("rotate_customer_kek", {{"customer_id", customer_id}});
    ASSERT_TRUE(finished.ok) << finished.error.message;
    EXPECT_EQ(finished.value.at("version"), 3);
    const auto done = store->customer_kek_state(customer_id);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->previous_version, 0U);
    const ApiResult second = call("read_secret", {{"mount_id", second_mount_id}, {"name", "b"}});
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("value").at("v"), "second");
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A lease record and route as lake writes them, with chosen instants (no waiting for a clock).
void place_lease(const ApiContext& context, const std::string& customer_id,
                 const std::string& workspace_id, const std::string& mount_id,
                 const std::string& lease_id, std::int64_t expires_us,
                 std::string revoked_at = {}) {
    auto records = lake::api::workspace_records(context, customer_id, workspace_id);
    ASSERT_TRUE(records) << records.error().message;
    lake::control::SecretLeaseRecord lease;
    lease.id = lease_id;
    lease.mount_id = mount_id;
    lease.role = "reader";
    lease.ciphertext = "c2VhbGVkLWNyZWRlbnRpYWw";
    lease.expires_us = expires_us;
    lease.revoked_at = std::move(revoked_at);
    ASSERT_TRUE(context.store->record_secret_lease_route(
        {lease_id, mount_id, customer_id, workspace_id,
         lease.revoked_at.empty() ? expires_us : now_us() - 1}));
    const auto written = lake::control::settled(liquid::runtime::block_on(records->put(
        lake::control::SecretLeaseCodec::collection, lease_id,
        lake::control::SecretLeaseCodec::lookup(mount_id),
        lake::control::SecretLeaseCodec::encode(lease),
        lake::control::RecordCondition{.if_absent = true}, 1, expires_us - 60'000'000)));
    ASSERT_TRUE(written) << written.error().message;
}

std::optional<lake::control::SecretLeaseRecord> lease_record(const ApiContext& context,
                                                             const std::string& customer_id,
                                                             const std::string& workspace_id,
                                                             const std::string& lease_id) {
    auto records = lake::api::workspace_records(context, customer_id, workspace_id);
    EXPECT_TRUE(records) << records.error().message;
    const auto got = lake::control::settled(liquid::runtime::block_on(
        records->get(lake::control::SecretLeaseCodec::collection, lease_id)));
    EXPECT_TRUE(got) << got.error().message;
    return got && *got ? lake::control::SecretLeaseCodec::decode(**got) : std::nullopt;
}

lake::api::SecretLeaseSweep sweep(const ApiContext& context) {
    const auto swept = liquid::runtime::block_on(lake::api::expire_secret_leases(context));
    EXPECT_TRUE(swept);
    return swept ? *swept : lake::api::SecretLeaseSweep{};
}

// lake#362: a lease is a sealed record of its mount's workspace; the control store keeps only its
// content-free route, which is the expiry loop's due index.
TEST_F(SecretsCapabilities, DynamicLeasesLiveInTheMountsWorkspace) {
    const ApiResult leased =
        call("lease_dynamic_secret",
             {{"mount_id", mount_id}, {"role", "reader"}, {"lifetime_seconds", 60}});
    ASSERT_TRUE(leased.ok) << leased.error.message;
    const std::string password = leased.value.at("value").at("password");
    const std::string username = leased.value.at("value").at("username");
    const std::string lease_id = leased.value.at("lease").at("id");
    EXPECT_EQ(leased.value.at("lease").at("workspace_id"), workspace_id);
    EXPECT_EQ(leased.value.at("lease").at("renewals"), 0);

    auto& rows = workspaces.records(workspace_id).rows("secrets_dynamic_leases");
    ASSERT_EQ(rows.size(), 1U);
    const std::string stored = rows.begin()->second.dump();
    EXPECT_EQ(stored.find(password), std::string::npos);
    EXPECT_EQ(stored.find(username), std::string::npos);
    EXPECT_EQ(stored.find("reader"), std::string::npos);
    auto record = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->ciphertext);
    EXPECT_EQ(record->ciphertext->find(password), std::string::npos);
    const auto route = store->get_secret_lease_route(lease_id);
    ASSERT_TRUE(route);
    EXPECT_EQ(route->mount_id, mount_id);
    EXPECT_EQ(route->workspace_id, workspace_id);
    EXPECT_EQ(route->due_us, record->expires_us) << "due exactly when the lease expires";

    const ApiResult renewed =
        call("renew_dynamic_secret", {{"lease_id", lease_id}, {"lifetime_seconds", 600}});
    ASSERT_TRUE(renewed.ok) << renewed.error.message;
    EXPECT_EQ(renewed.value.at("renewals"), 1);
    record = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(record);
    EXPECT_GT(record->expires_us, route->due_us);
    EXPECT_EQ(store->get_secret_lease_route(lease_id)->due_us, record->expires_us)
        << "the due index follows a renewal";
    EXPECT_EQ(call("renew_dynamic_secret", {{"lease_id", lease_id}, {"lifetime_seconds", 3601}})
                  .error.code,
              "invalid_input");

    const ApiResult revoked = call("revoke_dynamic_secret", {{"lease_id", lease_id}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    EXPECT_FALSE(revoked.value.at("revoked_at").is_null());
    record = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->ciphertext) << "a revoked credential's sealed value is erased";
    EXPECT_EQ(store->get_secret_lease_route(lease_id)->due_us, 0) << "settled";
    const ApiResult again = call("revoke_dynamic_secret", {{"lease_id", lease_id}});
    ASSERT_TRUE(again.ok) << "revocation is idempotent";
    EXPECT_EQ(again.value.at("revoked_at"), revoked.value.at("revoked_at"));
    const ApiResult refused = call("renew_dynamic_secret", {{"lease_id", lease_id}});
    EXPECT_EQ(refused.error.code, "conflict") << "a revoked lease is never renewed";

    for (const std::string bad : {std::string("not-a-lease"), lake::control::new_uuid_v4()}) {
        for (const std::string capability : {"renew_dynamic_secret", "revoke_dynamic_secret"}) {
            const ApiResult missing = call(capability, {{"lease_id", bad}});
            EXPECT_EQ(missing.error.code, "not_found") << capability << " " << bad;
            EXPECT_EQ(missing.error.path, "/lease_id") << capability << " " << bad;
        }
    }
    EXPECT_EQ(call("revoke_dynamic_secret", json::object()).error.code, "invalid_input");
    EXPECT_EQ(call("lease_dynamic_secret",
                   {{"mount_id", lake::control::new_uuid_v4()}, {"role", "reader"}})
                  .error.code,
              "not_found");
    EXPECT_EQ(call("lease_dynamic_secret",
                   {{"mount_id", mount_id}, {"role", "reader"}, {"lifetime_seconds", 0}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(rows.size(), 1U) << "a refused lease writes nothing";
}

// A renewal that read the lease active loses to a revocation or an expiry written meanwhile: its
// write is conditional on the revision it read, so it rereads and finds the lease settled.
TEST_F(SecretsCapabilities, ARenewalRacingARevocationOrAnExpiryCannotReviveTheLease) {
    auto& tables = workspaces.records(workspace_id);
    for (const std::string settle : {"revoke", "expire"}) {
        const ApiResult leased =
            call("lease_dynamic_secret",
                 {{"mount_id", mount_id}, {"role", "writer"}, {"lifetime_seconds", 60}});
        ASSERT_TRUE(leased.ok) << leased.error.message;
        const std::string lease_id = leased.value.at("lease").at("id");
        auto& rows = tables.rows("secrets_dynamic_leases");
        const json active = rows.at(lease_id);
        // The settled state another lake process writes.
        if (settle == "revoke") {
            ASSERT_TRUE(call("revoke_dynamic_secret", {{"lease_id", lease_id}}).ok);
        } else {
            auto records = lake::api::workspace_records(context, customer_id, workspace_id);
            ASSERT_TRUE(records);
            auto record = lease_record(context, customer_id, workspace_id, lease_id);
            ASSERT_TRUE(record);
            record->expires_us = now_us() - 1;
            ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(records->put(
                lake::control::SecretLeaseCodec::collection, lease_id,
                lake::control::SecretLeaseCodec::lookup(mount_id),
                lake::control::SecretLeaseCodec::encode(*record),
                lake::control::RecordCondition{.if_revision = record->revision},
                record->lake_revision + 1, record->at_us))));
            ASSERT_TRUE(store->reschedule_secret_lease(lease_id, now_us() - 1));
            const auto swept = sweep(context);
            EXPECT_EQ(swept.expired, 1U);
        }
        const json settled = rows.at(lease_id);
        // The renewal reads the active copy; the settled one lands before its write.
        rows[lease_id] = active;
        tables.before_put[lease_id] = [lease_id, settled](lake::test::RecordTables& tables) {
            json landed = settled;
            landed["revision"] = ++tables.clock;
            tables.rows("secrets_dynamic_leases")[lease_id] = landed;
        };
        const ApiResult renewed = call("renew_dynamic_secret", {{"lease_id", lease_id}});
        EXPECT_EQ(renewed.error.code, "conflict") << settle;
        EXPECT_TRUE(tables.before_put.empty()) << settle << ": the renewal did try to write";
        const auto record = lease_record(context, customer_id, workspace_id, lease_id);
        ASSERT_TRUE(record) << settle;
        EXPECT_TRUE(record->settled()) << settle;
        EXPECT_FALSE(record->ciphertext) << settle;
        EXPECT_EQ(record->renewals, 0) << settle;
        EXPECT_EQ(store->get_secret_lease_route(lease_id)->due_us, 0) << settle;
    }
}

// The expiry loop reads the control store's due index and only the workspaces that hold a due
// lease: a workspace whose leases are all active is not called at all.
TEST_F(SecretsCapabilities, TheExpiryLoopReadsOnlyTheWorkspacesOfDueLeases) {
    const std::string quiet_workspace = workspaces.create(customer_id, "quiet");
    const std::string slow_workspace = workspaces.create(customer_id, "slow");
    const auto mount_in = [&](const std::string& workspace, const std::string& name) {
        const ApiResult mount = call("create_secret_mount",
                                     {{"customer_id", customer_id},
                                      {"workspace_id", workspace},
                                      {"organization_id", organization_id},
                                      {"name", name}});
        EXPECT_TRUE(mount.ok) << mount.error.message;
        return mount.value.at("id").get<std::string>();
    };
    const std::string quiet_mount = mount_in(quiet_workspace, "quiet");
    const std::string slow_mount = mount_in(slow_workspace, "slow");
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(call("lease_dynamic_secret",
                         {{"mount_id", quiet_mount}, {"role", "reader"}, {"lifetime_seconds", 600}})
                        .ok);
    }
    const std::int64_t past = now_us() - 1'000'000;
    const std::string expired = lake::control::new_uuid_v4();
    const std::string revoked = lake::control::new_uuid_v4();
    const std::string renewed = lake::control::new_uuid_v4();
    const std::string waiting = lake::control::new_uuid_v4();
    place_lease(context, customer_id, workspace_id, mount_id, expired, past);
    place_lease(context, customer_id, workspace_id, mount_id, revoked, now_us() + 600'000'000,
                lake::control::record_instant(past));
    // Renewed after it was indexed: the index is earlier than the record.
    place_lease(context, customer_id, workspace_id, mount_id, renewed, now_us() + 600'000'000);
    ASSERT_TRUE(store->reschedule_secret_lease(renewed, past));
    place_lease(context, customer_id, slow_workspace, slow_mount, waiting, past);
    // A route whose record has not been written yet (a lease being created) is kept.
    const std::string unwritten = lake::control::new_uuid_v4();
    ASSERT_TRUE(store->record_secret_lease_route(
        {unwritten, mount_id, customer_id, workspace_id, past}));

    workspaces.status(slow_workspace, "Provisioning");
    workspaces.records(quiet_workspace).calls.clear();
    workspaces.records(workspace_id).calls.clear();
    const auto first = sweep(context);
    EXPECT_EQ(first.claimed, 5U);
    EXPECT_EQ(first.expired, 1U);
    EXPECT_EQ(first.settled, 1U) << "the revoked lease";
    EXPECT_EQ(first.rescheduled, 1U);
    EXPECT_EQ(first.failed, 2U) << "the not-ready workspace's lease and the unwritten route";
    EXPECT_EQ(first.removed, 0U);
    EXPECT_TRUE(workspaces.records(quiet_workspace).calls.empty())
        << "a workspace without due leases is never read";
    EXPECT_FALSE(workspaces.records(workspace_id).calls.empty());

    const auto expired_record = lease_record(context, customer_id, workspace_id, expired);
    ASSERT_TRUE(expired_record);
    EXPECT_FALSE(expired_record->expired_at.empty());
    EXPECT_FALSE(expired_record->ciphertext) << "an expired credential's sealed value is erased";
    EXPECT_EQ(store->get_secret_lease_route(expired)->due_us, 0);
    EXPECT_EQ(call("renew_dynamic_secret", {{"lease_id", expired}}).error.code, "conflict");
    const ApiResult revoke_expired = call("revoke_dynamic_secret", {{"lease_id", expired}});
    ASSERT_TRUE(revoke_expired.ok) << revoke_expired.error.message;
    const auto revoked_record = lease_record(context, customer_id, workspace_id, revoked);
    ASSERT_TRUE(revoked_record);
    EXPECT_FALSE(revoked_record->ciphertext);
    EXPECT_TRUE(revoked_record->expired_at.empty());
    const auto renewed_record = lease_record(context, customer_id, workspace_id, renewed);
    ASSERT_TRUE(renewed_record && renewed_record->ciphertext) << "an active lease is untouched";
    EXPECT_EQ(store->get_secret_lease_route(renewed)->due_us, renewed_record->expires_us);

    // Held: a second pass right away claims nothing, so two replicas do not both take a lease.
    EXPECT_EQ(sweep(context).claimed, 0U);
    // The not-ready workspace's lease is settled once it is ready and its hold has passed.
    workspaces.ready(slow_workspace);
    ASSERT_TRUE(store->reschedule_secret_lease(waiting, past));
    const auto second = sweep(context);
    EXPECT_EQ(second.expired, 1U);
    EXPECT_FALSE(lease_record(context, customer_id, slow_workspace, waiting)->ciphertext);

    // A deleted workspace took its leases with it: their routes are settled without a call.
    const std::string doomed = lake::control::new_uuid_v4();
    place_lease(context, customer_id, slow_workspace, slow_mount, doomed, past);
    ASSERT_TRUE(store->update_workspace(slow_workspace, {.desired = "deleted"}));
    workspaces.records(slow_workspace).calls.clear();
    const auto third = sweep(context);
    EXPECT_EQ(third.settled, 1U);
    EXPECT_TRUE(workspaces.records(slow_workspace).calls.empty());
    EXPECT_EQ(store->get_secret_lease_route(doomed)->due_us, 0);
}

// A renewal that lands between the loop's read and its write wins: the loop rereads, finds the
// lease active and moves its due instant to the renewed expiry.
TEST_F(SecretsCapabilities, ARenewalThatBeatsTheExpiryLoopIsKept) {
    const std::string lease_id = lake::control::new_uuid_v4();
    place_lease(context, customer_id, workspace_id, mount_id, lease_id, now_us() - 1);
    auto& tables = workspaces.records(workspace_id);
    const std::int64_t renewed_until = now_us() + 600'000'000;
    // The renewed copy, as another process's conditional write leaves it.
    const json before = tables.rows("secrets_dynamic_leases").at(lease_id);
    auto record = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(record);
    record->expires_us = renewed_until;
    record->renewals = 1;
    {
        auto records = lake::api::workspace_records(context, customer_id, workspace_id);
        ASSERT_TRUE(records);
        ASSERT_TRUE(lake::control::settled(liquid::runtime::block_on(records->put(
            lake::control::SecretLeaseCodec::collection, lease_id,
            lake::control::SecretLeaseCodec::lookup(mount_id),
            lake::control::SecretLeaseCodec::encode(*record),
            lake::control::RecordCondition{.if_revision = record->revision},
            record->lake_revision + 1, record->at_us))));
    }
    const json renewed = tables.rows("secrets_dynamic_leases").at(lease_id);
    tables.rows("secrets_dynamic_leases")[lease_id] = before;
    tables.before_put[lease_id] = [lease_id, renewed](lake::test::RecordTables& tables) {
        json landed = renewed;
        landed["revision"] = ++tables.clock;
        tables.rows("secrets_dynamic_leases")[lease_id] = landed;
    };
    const auto swept = sweep(context);
    EXPECT_TRUE(tables.before_put.empty()) << "the loop did try to expire it";
    EXPECT_EQ(swept.expired, 0U);
    EXPECT_EQ(swept.rescheduled, 1U);
    const auto kept = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(kept);
    EXPECT_TRUE(kept->active(now_us()));
    EXPECT_TRUE(kept->ciphertext);
    EXPECT_EQ(kept->renewals, 1);
    EXPECT_EQ(store->get_secret_lease_route(lease_id)->due_us, renewed_until);
}

// The server's owner loop runs the expiry pass on its tick (drain()).
TEST_F(SecretsCapabilities, TheServersOwnerLoopSettlesDueLeases) {
    const std::string lease_id = lake::control::new_uuid_v4();
    place_lease(context, customer_id, workspace_id, mount_id, lease_id, now_us() - 1);
    lake::api::ServerOptions options;
    options.port = 0;
    options.node_name = "lease-expiry";
    options.admin_token = "lease-expiry-admin";
    options.instance_kek = std::string(kek);
    options.store = store;
    options.workspaces = workspaces.service;
    options.secret_lease_expiry_interval = std::chrono::milliseconds(0);
    lake::api::ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    server.drain();
    const auto record = lease_record(context, customer_id, workspace_id, lease_id);
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->expired_at.empty());
    EXPECT_FALSE(record->ciphertext);
    EXPECT_EQ(store->get_secret_lease_route(lease_id)->due_us, 0);
    server.stop();
}

TEST_F(SecretsCapabilities, MissingMountGrantIsDeniedAndAudited) {
    ASSERT_TRUE(call("create_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}}).ok);
    context.operator_credential = false;
    context.acting_identity = "taap:1:ed25519:ungranted";
    const ApiResult denied_list = call("list_encryption_keys", {{"mount_id", mount_id}});
    EXPECT_FALSE(denied_list.ok);
    EXPECT_EQ(denied_list.error.code, "unauthorized");
    const ApiResult denied = call("read_secret", {{"mount_id", mount_id}, {"name", "database"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.outcome, "denied");
    EXPECT_EQ(event->draft.customer_id, customer_id);
    EXPECT_EQ(event->draft.object, "secret_mount/" + mount_id);
    EXPECT_EQ(event->draft.details.at("mount_id"), mount_id);
    EXPECT_EQ(event->draft.details.at("key"), "database");
    // The ungranted caller sees no mount in the list, and reaches no workspace to learn that.
    const auto calls = workspaces.records(workspace_id).calls.size();
    const ApiResult listed = call("list_secret_mounts",
                                  {{"customer_id", customer_id}, {"workspace_id", workspace_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_TRUE(listed.value.at("items").empty());
    EXPECT_EQ(workspaces.records(workspace_id).calls.size(), calls);
}

} // namespace
