#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using lake::api::Registry;
using nlohmann::json;

class SecretsCapabilities : public ::testing::Test {
protected:
    void SetUp() override {
        lake::api::register_core_capabilities(registry, "test");
        context.store = &store;
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = "memory-test-key";

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
        const ApiResult mount = call("create_secret_mount",
                                     {{"customer_id", customer_id},
                                      {"organization_id", organization_id},
                                      {"name", "application"},
                                      {"description", "Application credentials"}});
        ASSERT_TRUE(mount.ok) << mount.error.message;
        mount_id = mount.value.at("id");
    }

    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id.clear();
        return registry.call(name, input, context);
    }

    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext context;
    std::string organization_id;
    std::string customer_id;
    std::string mount_id;
};

TEST_F(SecretsCapabilities, VersionHistoryDeleteUndeleteAndDestroy) {
    ASSERT_TRUE(call("get_secret_mount", {{"mount_id", mount_id}}).ok);
    const ApiResult mounts = call("list_secret_mounts", {{"customer_id", customer_id}});
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
    const ApiResult destroyed_history =
        call("secret_metadata", {{"mount_id", mount_id}, {"name", "database"}});
    ASSERT_TRUE(destroyed_history.ok) << destroyed_history.error.message;
    EXPECT_FALSE(destroyed_history.value.at("versions").at(0).at("destroyed_at").is_null());
}

TEST_F(SecretsCapabilities, RotationRewrapSigningHmacDerivationAndKekRotation) {
    const ApiResult created =
        call("create_encryption_key", {{"mount_id", mount_id}, {"name", "orders"}});
    ASSERT_TRUE(created.ok) << created.error.message;
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
    const ApiResult kek = call("rotate_customer_kek", {{"customer_id", customer_id}});
    ASSERT_TRUE(kek.ok) << kek.error.message;
    EXPECT_EQ(kek.value.at("version"), 2);
    EXPECT_GE(kek.value.at("data_keys_rewrapped"), 4);
    const ApiResult after = call("read_secret", {{"mount_id", mount_id}, {"name", "stable"}});
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value.at("value").at("token"), "unchanged");
    const ApiResult rewrapped_decrypted = call(
        "decrypt",
        {{"mount_id", mount_id}, {"name", "orders"}, {"ciphertext", rewrapped.value.at("value")}});
    ASSERT_TRUE(rewrapped_decrypted.ok) << rewrapped_decrypted.error.message;
    EXPECT_EQ(rewrapped_decrypted.value.at("value"), payload);
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
    const auto event = store.latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.outcome, "denied");
    EXPECT_EQ(event->draft.customer_id, customer_id);
    EXPECT_EQ(event->draft.object, "secret_mount/" + mount_id);
    EXPECT_EQ(event->draft.details.at("mount_id"), mount_id);
    EXPECT_EQ(event->draft.details.at("key"), "database");
}

} // namespace
