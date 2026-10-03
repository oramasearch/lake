// Static secrets and named encryption keys (docs/api/secrets.md), kept in the workspace Zetta of
// their mount (lake#328-#332): version history, delete/undelete/destroy, transit operations, the
// customer KEK rotation across workspaces, and the conditional writes that keep versions unique.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "api/secret_records_api.hpp"
#include "api/workspace_records_api.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
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

TEST_F(SecretsCapabilities, DynamicLeasesUseTheMountDataKeyInItsWorkspace) {
    const ApiResult leased =
        call("lease_dynamic_secret",
             {{"mount_id", mount_id}, {"role", "reader"}, {"lifetime_seconds", 60}});
    ASSERT_TRUE(leased.ok) << leased.error.message;
    EXPECT_TRUE(leased.value.at("value").contains("password"));
    const std::string lease_id = leased.value.at("lease").at("id");
    ASSERT_TRUE(call("renew_dynamic_secret", {{"lease_id", lease_id}}).ok);
    ASSERT_TRUE(call("revoke_dynamic_secret", {{"lease_id", lease_id}}).ok);
    EXPECT_EQ(call("lease_dynamic_secret",
                   {{"mount_id", lake::control::new_uuid_v4()}, {"role", "reader"}})
                  .error.code,
              "not_found");
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
