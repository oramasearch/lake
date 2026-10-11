// A user's workspace secret delivered to a Kubernetes Secret (lake#625): the sealed-secret
// envelope version 1 byte for byte against the orama-operator's reference fixture, and
// mint_sealed_secret, the operator's only way to a value (docs/api/secrets.md#projections).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/sealed_delivery.hpp"
#include "lake/control/memory_store.hpp"
#include "workspace_fixture.hpp"

namespace {

using lake::api::ApiContext;
using lake::api::ApiResult;
using nlohmann::json;

constexpr std::string_view kek = "7365637265747320696e20776f726b7370616365732074657374206b656b2121";

template <std::size_t N> std::array<std::uint8_t, N> sequence(std::uint8_t start) {
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<std::uint8_t>(start + i);
    }
    return out;
}

std::array<std::uint8_t, 32> public_of(const std::array<std::uint8_t, 32>& private_key) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), 32);
    std::array<std::uint8_t, 32> out{};
    std::size_t length = out.size();
    EXPECT_EQ(EVP_PKEY_get_raw_public_key(key, out.data(), &length), 1);
    EVP_PKEY_free(key);
    return out;
}

std::string base64url(const std::array<std::uint8_t, 32>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 63];
        }
    }
    if (bits > 0) {
        out += alphabet[(buffer << (6 - bits)) & 63];
    }
    return out;
}

std::string decoded(const std::string& base64) {
    std::string out(base64.size(), '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       reinterpret_cast<const unsigned char*>(base64.data()),
                                       static_cast<int>(base64.size()));
    EXPECT_GT(length, 0);
    out.resize(static_cast<std::size_t>(std::max(length, 0)));
    return out;
}

// The operator's TestSealedSecretEnvelopeReferenceFixture (orama-operator
// internal/controller/lakesealedsecret_controller_test.go): the same inputs give the same bytes,
// so the operator opens what lake seals.
TEST(SealedDelivery, TheEnvelopeIsTheOperatorsReferenceFixtureByteForByte) {
    const auto recipient_private = sequence<32>(1);
    lake::api::SealingRecipient recipient;
    recipient.cluster = "cluster-fixture";
    recipient.namespace_name = "payments";
    recipient.name = "database";
    recipient.public_key = public_of(recipient_private);
    recipient.kid = lake::api::sealing_kid(recipient.public_key);
    lake::api::SealingRandomness fixed{sequence<32>(33), sequence<24>(65)};
    const auto sealed = lake::api::seal_for_cluster(recipient, "fixture-secret", &fixed);
    ASSERT_TRUE(sealed) << sealed.error();
    EXPECT_EQ(*sealed,
              "pmF2AWJjdFgeEHhaBXET+DLyNpH7FrrQcmyA2bKdKakJ+AEZcFR7Y2FhZKNkbmFtZWhkYXRhYmFzZWdjbHV"
              "zdGVyb2NsdXN0ZXItZml4dHVyZWluYW1lc3BhY2VocGF5bWVudHNjZXBrWCBYaa/0UFSXMsuq7V5d+bMKba"
              "McsOV0K61a1KGnaPGme2NraWR2cXFqXzl3TzFDeUtYOVBiaE5RajNKQWVub25jZVgYQUJDREVGR0hJSktM"
              "TU5PUFFSU1RVVldY");
    // Fresh randomness: a different record for the same value, never the value in the clear.
    const auto first = lake::api::seal_for_cluster(recipient, "fixture-secret");
    const auto second = lake::api::seal_for_cluster(recipient, "fixture-secret");
    ASSERT_TRUE(first && second);
    EXPECT_NE(*first, *second);
    EXPECT_EQ(decoded(*first).find("fixture-secret"), std::string::npos);
    // The public key round-trips through the form Lake status.sealing.publicKey carries.
    const auto key = lake::api::decode_sealing_public_key(base64url(recipient.public_key));
    ASSERT_TRUE(key) << key.error();
    EXPECT_EQ(*key, recipient.public_key);
    EXPECT_FALSE(lake::api::decode_sealing_public_key("not base64url!"));
    EXPECT_FALSE(lake::api::decode_sealing_public_key("AAAA"));
}

TEST(SealedDelivery, ALakeSecretReferenceNamesMountSecretAndField) {
    const auto parsed =
        lake::api::parse_lake_secret_ref("lake-secret://0b5f/payments/db#prod#DATABASE_URL");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->mount_id, "0b5f");
    EXPECT_EQ(parsed->name, "payments/db#prod");
    EXPECT_EQ(parsed->field, "DATABASE_URL");
    EXPECT_EQ(lake::api::lake_secret_ref(*parsed),
              "lake-secret://0b5f/payments/db#prod#DATABASE_URL");
    for (const char* bad : {"db-credentials",
                            "db-credentials/password",
                            "lake-secret://mount/name",
                            "lake-secret://mount#field",
                            "lake-secret:///name#field",
                            "lake-secret://mount/#field",
                            "lake-secret://mount/name#",
                            "lake-secret://mount/name#bad field"}) {
        EXPECT_FALSE(lake::api::parse_lake_secret_ref(bad)) << bad;
    }
    EXPECT_TRUE(lake::api::kubernetes_secret_key("tls.crt"));
    EXPECT_FALSE(lake::api::kubernetes_secret_key(".."));
    EXPECT_FALSE(lake::api::kubernetes_secret_key("a/b"));
}

class MintSealedSecret : public ::testing::Test {
protected:
    void SetUp() override {
        lake::api::register_core_capabilities(registry, "test");
        context.store = store.get();
        context.workspaces = workspaces.service.get();
        context.registry = &registry;
        context.authorized = true;
        context.operator_credential = true;
        context.acting_identity = "admin";
        context.instance_kek = std::string(kek);
        const ApiResult organization =
            call("create_organization", {{"display_name", "Delivery organization"}});
        ASSERT_TRUE(organization.ok) << organization.error.message;
        const ApiResult customer = call("create_customer",
                                        {{"customer_type", "organization"},
                                         {"organization_id", organization.value.at("id")},
                                         {"name", "delivery-customer"}});
        ASSERT_TRUE(customer.ok) << customer.error.message;
        customer_id = customer.value.at("id");
        workspace_id = workspaces.create(customer_id, "apps");
        const ApiResult mount = call("create_secret_mount",
                                     {{"customer_id", customer_id},
                                      {"workspace_id", workspace_id},
                                      {"organization_id", organization.value.at("id")},
                                      {"name", "application"}});
        ASSERT_TRUE(mount.ok) << mount.error.message;
        mount_id = mount.value.at("id");
        recipient_public = public_of(sequence<32>(7));
        context.sealing_public_key = base64url(recipient_public);
    }

    ApiResult call(const std::string& name, json input = json::object()) {
        context.request_id.clear();
        context.audit_workspace_id.clear();
        context.audit_customer_id.clear();
        const auto completed = liquid::runtime::block_on(registry.call_async(name, input, context));
        EXPECT_TRUE(completed);
        return completed ? *completed : ApiResult::failure("store_error", "did not complete");
    }

    json recipient(std::string name = "app-1-env") const {
        return {{"cluster", "8cc182a1-5a84-4c65-92c7-742481ec88d4"},
                {"namespace", "prod-ws-1234"},
                {"name", std::move(name)},
                {"kid", lake::api::sealing_kid(recipient_public)},
                {"public_key", base64url(recipient_public)}};
    }

    json mint_input(json source) const {
        return {{"attestation",
                 {{"type", "kubernetes"}, {"token", "projected"}, {"audiences", {"lake"}}}},
                {"recipient", recipient()},
                {"source", std::move(source)}};
    }

    lake::api::Registry registry;
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    lake::test::RecordWorkspaces workspaces{store};
    ApiContext context;
    std::string customer_id;
    std::string workspace_id;
    std::string mount_id;
    std::array<std::uint8_t, 32> recipient_public{};
};

TEST_F(MintSealedSecret, DeliversEveryFieldSealedNeverAValueAndFollowsRotation) {
    const std::string url = "postgres://app:s3cr3t-v1@db/app";
    ASSERT_TRUE(call("put_secret",
                     {{"mount_id", mount_id},
                      {"name", "database"},
                      {"value", {{"DATABASE_URL", url}, {"PORT", 5432}}}})
                    .ok);
    const json source{{"type", "lake"}, {"lake", {{"mount", mount_id}, {"name", "database"}}}};
    const ApiResult minted = call("mint_sealed_secret", mint_input(source));
    ASSERT_TRUE(minted.ok) << minted.error.message;
    EXPECT_EQ(minted.value.at("version"), 1);
    const json& data = minted.value.at("data");
    ASSERT_EQ(data.size(), 2U);
    for (const auto& [key, record] : data.items()) {
        const std::string bytes = decoded(record.get<std::string>());
        EXPECT_NE(bytes.find("prod-ws-1234"), std::string::npos) << key; // bound to its namespace
        EXPECT_NE(bytes.find("app-1-env"), std::string::npos) << key;    // and its Secret
        EXPECT_EQ(bytes.find("s3cr3t"), std::string::npos) << key;
    }
    EXPECT_EQ(minted.value.dump().find("s3cr3t"), std::string::npos);
    // Audited against the mount, naming the target Secret and never a value.
    const auto event = store->latest_audit_event();
    ASSERT_TRUE(event) << event.error().message;
    EXPECT_EQ(event->draft.object, "secret_mount/" + mount_id);
    EXPECT_EQ(event->draft.details.at("namespace"), "prod-ws-1234");
    EXPECT_EQ(event->draft.details.at("secret"), "app-1-env");
    EXPECT_EQ(event->draft.details.dump().find("s3cr3t"), std::string::npos);
    // Rotation: a new version is what the next delivery carries.
    ASSERT_TRUE(
        call("put_secret",
             {{"mount_id", mount_id},
              {"name", "database"},
              {"value", {{"DATABASE_URL", "postgres://app:s3cr3t-v2@db/app"}, {"PORT", 5432}}}})
            .ok);
    const ApiResult rotated = call("mint_sealed_secret", mint_input(source));
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_EQ(rotated.value.at("version"), 2);
    // A pinned version stays pinned.
    json pinned = source;
    pinned["lake"]["version"] = 1;
    const ApiResult old = call("mint_sealed_secret", mint_input(pinned));
    ASSERT_TRUE(old.ok) << old.error.message;
    EXPECT_EQ(old.value.at("version"), 1);
}

TEST_F(MintSealedSecret, OnlyTheOperatorAndOnlyToThisClustersKey) {
    ASSERT_TRUE(
        call("put_secret",
             {{"mount_id", mount_id}, {"name", "database"}, {"value", {{"password", "pw-1"}}}})
            .ok);
    const json source{{"type", "lake"}, {"lake", {{"mount", mount_id}, {"name", "database"}}}};

    // A person, even one who may read the mount, never gets a delivery.
    context.operator_credential = false;
    context.acting_identity = "taap:1:ed25519:someone";
    const ApiResult person = call("mint_sealed_secret", mint_input(source));
    EXPECT_FALSE(person.ok);
    EXPECT_EQ(person.error.code, "unauthorized");
    context.operator_credential = true;
    context.acting_identity = "admin";

    // Another key than the cluster's is refused, as is a kid that does not name the key.
    json foreign = mint_input(source);
    const auto other = public_of(sequence<32>(99));
    foreign["recipient"]["public_key"] = base64url(other);
    foreign["recipient"]["kid"] = lake::api::sealing_kid(other);
    const ApiResult refused = call("mint_sealed_secret", foreign);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.path, "/recipient/public_key");
    json mislabelled = mint_input(source);
    mislabelled["recipient"]["kid"] = "not-the-kid";
    EXPECT_EQ(call("mint_sealed_secret", mislabelled).error.path, "/recipient/kid");

    // An unknown secret, an unknown mount and a field that cannot be a Secret key.
    EXPECT_EQ(
        call("mint_sealed_secret",
             mint_input({{"type", "lake"}, {"lake", {{"mount", mount_id}, {"name", "missing"}}}}))
            .error.code,
        "not_found");
    EXPECT_FALSE(call("mint_sealed_secret",
                      mint_input({{"type", "lake"},
                                  {"lake",
                                   {{"mount", "00000000-0000-4000-8000-000000000000"},
                                    {"name", "database"}}}}))
                     .ok);
    ASSERT_TRUE(
        call("put_secret", {{"mount_id", mount_id}, {"name", "odd"}, {"value", {{"a/b", "x"}}}})
            .ok);
    EXPECT_EQ(call("mint_sealed_secret",
                   mint_input({{"type", "lake"}, {"lake", {{"mount", mount_id}, {"name", "odd"}}}}))
                  .error.code,
              "invalid_input");
    EXPECT_EQ(call("mint_sealed_secret", mint_input({{"type", "literal"}})).error.path,
              "/source/type");
}

TEST_F(MintSealedSecret, ADynamicSourceLeasesFromTheMount) {
    const ApiResult minted = call(
        "mint_sealed_secret",
        mint_input({{"type", "dynamic"},
                    {"dynamic", {{"engine", mount_id}, {"role", "reader"}, {"ttl", "1h0m0s"}}}}));
    ASSERT_TRUE(minted.ok) << minted.error.message;
    EXPECT_TRUE(minted.value.at("data").contains("username"));
    EXPECT_TRUE(minted.value.at("data").contains("password"));
    EXPECT_FALSE(minted.value.at("lease").at("id").get<std::string>().empty());
    EXPECT_FALSE(minted.value.at("lease").at("expires_at").get<std::string>().empty());
    EXPECT_EQ(
        call("mint_sealed_secret",
             mint_input({{"type", "dynamic"},
                         {"dynamic", {{"engine", mount_id}, {"role", "reader"}, {"ttl", "2h"}}}}))
            .error.path,
        "/source/dynamic");
}

// An app's secret environment names a lake secret of its own workspace: a mount, a secret and a
// field, stored as a reference and never a value; the operator projects it at deploy (lake#625).
TEST_F(MintSealedSecret, AnAppNamesALakeSecretItsWorkspaceHolds) {
    context.apps_wildcard = "apps.example.test";
    ASSERT_TRUE(call("put_secret",
                     {{"mount_id", mount_id},
                      {"name", "database"},
                      {"value", {{"DATABASE_URL", "postgres://app:pw-app@db/app"}}}})
                    .ok);
    const json secret{{"mount_id", mount_id}, {"name", "database"}, {"field", "DATABASE_URL"}};
    const ApiResult created = call("create_app",
                                   {{"customer_id", customer_id},
                                    {"workspace_id", workspace_id},
                                    {"name", "reports"},
                                    {"mode", "image"},
                                    {"image", "registry.example.test/reports:v1"},
                                    {"framework", "container"},
                                    {"port", 8080},
                                    {"env", {{"DATABASE_URL", {{"secret", secret}}}}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string reference = "lake-secret://" + mount_id + "/database#DATABASE_URL";
    EXPECT_EQ(created.value.at("secret_env_refs").at("DATABASE_URL"), reference);
    EXPECT_EQ(created.value.dump().find("pw-app"), std::string::npos);
    const std::string app_id = created.value.at("id");

    // set_app_env takes the object or the reference string.
    const ApiResult by_string = call("set_app_env",
                                     {{"customer_id", customer_id},
                                      {"workspace_id", workspace_id},
                                      {"app_id", app_id},
                                      {"key", "DB"},
                                      {"secret_ref", reference}});
    ASSERT_TRUE(by_string.ok) << by_string.error.message;
    EXPECT_EQ(by_string.value.at("secret_env_refs").at("DB"), reference);

    // Refused: a Kubernetes Secret name, a missing field, a missing secret, a mount of another
    // workspace, and a value with no secret named.
    const auto set = [&](json extra) {
        json input{{"customer_id", customer_id},
                   {"workspace_id", workspace_id},
                   {"app_id", app_id},
                   {"key", "X"}};
        input.update(extra);
        return call("set_app_env", input);
    };
    EXPECT_EQ(set({{"secret_ref", "db-credentials/password"}}).error.code, "invalid_input");
    json missing_field = secret;
    missing_field["field"] = "PASSWORD";
    EXPECT_EQ(set({{"secret", missing_field}}).error.code, "not_found");
    json missing_secret = secret;
    missing_secret["name"] = "nothing";
    EXPECT_EQ(set({{"secret", missing_secret}}).error.code, "not_found");
    const std::string elsewhere = workspaces.create(customer_id, "elsewhere");
    const ApiResult other_mount =
        call("create_secret_mount",
             {{"customer_id", customer_id}, {"workspace_id", elsewhere}, {"name", "elsewhere"}});
    ASSERT_TRUE(other_mount.ok) << other_mount.error.message;
    ASSERT_TRUE(call("put_secret",
                     {{"mount_id", other_mount.value.at("id")},
                      {"name", "database"},
                      {"value", {{"DATABASE_URL", "x"}}}})
                    .ok);
    json foreign = secret;
    foreign["mount_id"] = other_mount.value.at("id");
    const ApiResult refused = set({{"secret", foreign}});
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(refused.error.path, "/secret");
    EXPECT_EQ(set({{"secret", secret}, {"value", "plain"}}).error.code, "invalid_input");
}

} // namespace
