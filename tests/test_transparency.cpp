#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/transparency/checkpoint.hpp"
#include "lake/transparency/tuf.hpp"

using nlohmann::json;

namespace {

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};

struct ContextDeleter {
    void operator()(EVP_MD_CTX* context) const {
        EVP_MD_CTX_free(context);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Context = std::unique_ptr<EVP_MD_CTX, ContextDeleter>;

std::string hex(std::span<const unsigned char> value) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        output.push_back(alphabet[byte >> 4]);
        output.push_back(alphabet[byte & 0x0f]);
    }
    return output;
}

std::string sha256(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    EXPECT_EQ(EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr),
              1);
    return hex(std::span(digest.data(), size));
}

struct TestKey {
    json value;
    std::string id;
    Pkey key;

    explicit TestKey(unsigned char marker) {
        std::array<unsigned char, 32> seed{};
        for (std::size_t index = 0; index < seed.size(); ++index) {
            seed[index] = static_cast<unsigned char>(marker + index);
        }
        key.reset(
            EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        EXPECT_NE(key, nullptr);
        std::array<unsigned char, 32> public_key{};
        std::size_t public_key_size = public_key.size();
        EXPECT_EQ(EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &public_key_size), 1);
        value = {{"keytype", "ed25519"},
                 {"scheme", "ed25519"},
                 {"keyval", {{"public", hex(public_key)}}}};
        id = sha256(value.dump());
    }

    TestKey(TestKey&&) = default;
    TestKey& operator=(TestKey&&) = default;
};

std::string sign(const TestKey& key, std::string_view value) {
    Context context(EVP_MD_CTX_new());
    EXPECT_NE(context, nullptr);
    EXPECT_EQ(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.key.get()), 1);
    std::array<unsigned char, 64> signature{};
    std::size_t size = signature.size();
    EXPECT_EQ(EVP_DigestSign(context.get(),
                             signature.data(),
                             &size,
                             reinterpret_cast<const unsigned char*>(value.data()),
                             value.size()),
              1);
    return hex(std::span(signature.data(), size));
}

json signed_document(json body, const std::vector<const TestKey*>& signers) {
    json signatures = json::array();
    const std::string encoded = body.dump();
    for (const TestKey* signer : signers) {
        signatures.push_back({{"keyid", signer->id}, {"sig", sign(*signer, encoded)}});
    }
    return {{"signed", std::move(body)}, {"signatures", std::move(signatures)}};
}

json role(const std::vector<const TestKey*>& keys, std::uint64_t threshold) {
    json key_ids = json::array();
    for (const TestKey* key : keys) {
        key_ids.push_back(key->id);
    }
    return {{"keyids", std::move(key_ids)}, {"threshold", threshold}};
}

json root_document(std::uint64_t version,
                   const std::vector<const TestKey*>& root_keys,
                   const std::vector<const TestKey*>& target_keys,
                   const TestKey& snapshot_key,
                   const TestKey& timestamp_key,
                   const std::vector<const TestKey*>& signers) {
    json keys = json::object();
    for (const TestKey* key : root_keys) {
        keys[key->id] = key->value;
    }
    for (const TestKey* key : target_keys) {
        keys[key->id] = key->value;
    }
    keys[snapshot_key.id] = snapshot_key.value;
    keys[timestamp_key.id] = timestamp_key.value;
    json body{{"_type", "root"},
              {"spec_version", "1.0.36"},
              {"version", version},
              {"expires", "2099-01-01T00:00:00Z"},
              {"consistent_snapshot", true},
              {"keys", std::move(keys)},
              {"roles",
               {{"root", role(root_keys, 2)},
                {"targets", role(target_keys, 2)},
                {"snapshot", role({&snapshot_key}, 1)},
                {"timestamp", role({&timestamp_key}, 1)}}}};
    return signed_document(std::move(body), signers);
}

json targets_document(std::uint64_t version,
                      const std::vector<const TestKey*>& signers,
                      std::string expires = "2099-01-01T00:00:00Z") {
    const std::string attestation = "TAAP";
    json body{{"_type", "targets"},
              {"spec_version", "1.0.36"},
              {"version", version},
              {"expires", std::move(expires)},
              {"targets",
               {{"attestations/entry.cbor",
                 {{"length", attestation.size()},
                  {"hashes", {{"sha256", sha256(attestation)}}},
                  {"custom",
                   {{"attestation_profile", "authorship/1"},
                    {"subject", "taap:1:content:sha512-256:fixture"}}}}}}}};
    return signed_document(std::move(body), signers);
}

json binding(const json& document) {
    const std::string encoded = document.dump();
    return {{"version", document.at("signed").at("version")},
            {"length", encoded.size()},
            {"hashes", {{"sha256", sha256(encoded)}}}};
}

json snapshot_document(std::uint64_t version,
                       const json& targets,
                       const TestKey& signer,
                       std::string expires = "2099-01-01T00:00:00Z") {
    json body{{"_type", "snapshot"},
              {"spec_version", "1.0.36"},
              {"version", version},
              {"expires", std::move(expires)},
              {"meta", {{"targets.json", binding(targets)}}}};
    return signed_document(std::move(body), {&signer});
}

json timestamp_document(std::uint64_t version,
                        const json& snapshot,
                        const TestKey& signer,
                        std::string expires = "2099-01-01T00:00:00Z") {
    json body{{"_type", "timestamp"},
              {"spec_version", "1.0.36"},
              {"version", version},
              {"expires", std::move(expires)},
              {"meta", {{"snapshot.json", binding(snapshot)}}}};
    return signed_document(std::move(body), {&signer});
}

struct Bundle {
    json targets;
    json snapshot;
    json timestamp;
};

Bundle bundle(std::uint64_t version,
              const std::vector<const TestKey*>& target_keys,
              const TestKey& snapshot_key,
              const TestKey& timestamp_key,
              std::string timestamp_expiry = "2099-01-01T00:00:00Z") {
    Bundle value;
    value.targets = targets_document(version, target_keys);
    value.snapshot = snapshot_document(version, value.targets, snapshot_key);
    value.timestamp =
        timestamp_document(version, value.snapshot, timestamp_key, std::move(timestamp_expiry));
    return value;
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        static std::atomic<unsigned long> next{0};
        path_ =
            std::filesystem::temp_directory_path() / ("lake-tuf-test-" + std::to_string(++next));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRoot() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct Keys {
    TestKey root_a{1};
    TestKey root_b{33};
    TestKey root_c{65};
    TestKey target_a{97};
    TestKey target_b{129};
    TestKey snapshot{161};
    TestKey timestamp{193};

    std::vector<const TestKey*> roots() const {
        return {&root_a, &root_b, &root_c};
    }

    std::vector<const TestKey*> root_threshold() const {
        return {&root_a, &root_b};
    }

    std::vector<const TestKey*> targets() const {
        return {&target_a, &target_b};
    }
};

json checkpoint_body() {
    return {{"root", "3b4f65a38e54aef7153fa97dbf4325d57f0d6d08712fdb3a8db8f06fe9ba6bde"},
            {"tree_size", 12},
            {"version", "lake_checkpoint/1"}};
}

lake::transparency::CheckpointSigner checkpoint_signer() {
    return [](const json&) -> lake::transparency::Result<json> {
        return json{{"algorithm", "ed25519"},
                    {"signer", "taap:1:ed25519:lake"},
                    {"signature", "local-signature"}};
    };
}

lake::transparency::LogConfiguration air_gapped_configuration() {
    return {.mode = lake::transparency::LogMode::AirGapped};
}

} // namespace

TEST(TransparencyCheckpoint, ValidatesRuntimeConfiguration) {
    const auto default_configuration =
        lake::transparency::log_configuration_from_environment("witnessed", "[]", 0, "");
    EXPECT_FALSE(default_configuration);
    EXPECT_NE(default_configuration.error().message.find("quorum-satisfiable"), std::string::npos);

    const auto anchored_without_aggregator = lake::transparency::log_configuration_from_environment(
        "anchored",
        R"([{"endpoint":"https://witness.example/checkpoint","public_key":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"}])",
        1,
        "");
    EXPECT_FALSE(anchored_without_aggregator);
    EXPECT_NE(anchored_without_aggregator.error().message.find("aggregation endpoint"),
              std::string::npos);

    const auto non_loopback_http = lake::transparency::log_configuration_from_environment(
        "witnessed",
        R"([{"endpoint":"http://localhost.example/checkpoint","public_key":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"}])",
        1,
        "");
    EXPECT_FALSE(non_loopback_http);
    EXPECT_NE(non_loopback_http.error().message.find("loopback"), std::string::npos);
}

TEST(TransparencyCheckpoint, ServerRefusesInvalidConfigurationAtStartup) {
    TemporaryRoot temporary;
    lake::api::ServerOptions options;
    options.store_root = temporary.path();
    options.node_name = "test-lake";
    options.log_configuration = {.mode = lake::transparency::LogMode::Witnessed};

    lake::api::ApiServer server(std::move(options));

    EXPECT_FALSE(server.start());
    EXPECT_NE(server.reason().find("quorum-satisfiable"), std::string::npos);
}

TEST(TransparencyCheckpoint, AirGappedSignsAndServesWithoutOutboundCalls) {
    TemporaryRoot temporary;
    lake::transparency::CheckpointService service(
        temporary.path(), "air-gapped", air_gapped_configuration());
    std::size_t calls = 0;
    const auto published = service.publish(
        checkpoint_body(), checkpoint_signer(), [&calls](const std::string&, const json&) {
            ++calls;
            return lake::transparency::Result<json>(json::object());
        });
    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(published->at("sequence"), 1);
    EXPECT_TRUE(published->at("witness_signatures").empty());
    EXPECT_EQ(published->at("signature").at("signer"), "taap:1:ed25519:lake");
    const auto current = service.current();
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(*current, *published);
    EXPECT_FALSE(std::filesystem::exists(service.directory() / "1.ots.json"));
}

TEST(TransparencyCheckpoint, WitnessedRequiresThresholdAndAnchoredStoresAggregationProof) {
    TemporaryRoot temporary;
    TestKey first{7};
    TestKey second{39};
    TestKey third{71};
    lake::transparency::LogConfiguration witnessed{
        .mode = lake::transparency::LogMode::Witnessed,
        .witnesses = {{"http://localhost:4901/witness",
                       first.value.at("keyval").at("public").get<std::string>()},
                      {"http://localhost:4902/witness",
                       second.value.at("keyval").at("public").get<std::string>()},
                      {"http://localhost:4903/witness",
                       third.value.at("keyval").at("public").get<std::string>()}},
        .witness_threshold = 2,
    };
    lake::transparency::CheckpointService witnessed_service(
        temporary.path(), "witnessed", witnessed);
    const auto witnessed_checkpoint = witnessed_service.publish(
        checkpoint_body(),
        checkpoint_signer(),
        [&first, &second](const std::string& endpoint,
                          const json& request) -> lake::transparency::Result<json> {
            const std::string body = request.at("checkpoint").dump();
            if (endpoint.ends_with("4901/witness")) {
                return json{{"signature", sign(first, body)}};
            }
            if (endpoint.ends_with("4902/witness")) {
                return json{{"signature", sign(second, body)}};
            }
            return json{{"signature", "not-a-signature"}};
        });
    ASSERT_TRUE(witnessed_checkpoint) << witnessed_checkpoint.error().message;
    EXPECT_EQ(witnessed_checkpoint->at("witness_signatures").size(), 2U);

    lake::transparency::CheckpointService refused_service(temporary.path(), "refused", witnessed);
    const auto refused = refused_service.publish(
        checkpoint_body(),
        checkpoint_signer(),
        [&first](const std::string&, const json& request) -> lake::transparency::Result<json> {
            return json{{"signature", sign(first, request.at("checkpoint").dump())}};
        });
    EXPECT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "unrecoverable");
    EXPECT_FALSE(refused_service.current());

    lake::transparency::LogConfiguration anchored = witnessed;
    anchored.mode = lake::transparency::LogMode::Anchored;
    anchored.aggregator_endpoint = "http://localhost:4904/aggregate";
    lake::transparency::CheckpointService anchored_service(temporary.path(), "anchored", anchored);
    const auto anchored_checkpoint = anchored_service.publish(
        checkpoint_body(),
        checkpoint_signer(),
        [&first, &second](const std::string& endpoint,
                          const json& request) -> lake::transparency::Result<json> {
            if (endpoint.ends_with("aggregate")) {
                return json{{"proof",
                             {{"calendar", "https://calendar.example"},
                              {"checkpoint_hash", request.at("hash")}}}};
            }
            const std::string body = request.at("checkpoint").dump();
            if (endpoint.ends_with("4901/witness")) {
                return json{{"signature", sign(first, body)}};
            }
            return json{{"signature", sign(second, body)}};
        });
    ASSERT_TRUE(anchored_checkpoint) << anchored_checkpoint.error().message;
    EXPECT_TRUE(anchored_checkpoint->contains("anchor"));
    EXPECT_TRUE(std::filesystem::exists(anchored_service.directory() / "1.ots.json"));
}

TEST(TransparencyTuf, AppendsThresholdSignedMetadataAndRejectsRepositoryAttacks) {
    TemporaryRoot temporary;
    Keys keys;
    lake::transparency::Service service(temporary.path(), "instance-a");
    const json root = root_document(
        1, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold());
    const auto initialized = service.initialize(root);
    ASSERT_TRUE(initialized) << initialized.error().message;
    EXPECT_EQ(initialized->version, 1U);

    const Bundle first = bundle(1, keys.targets(), keys.snapshot, keys.timestamp);
    const auto published = service.publish(first.targets, first.snapshot, first.timestamp);
    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(published->at("targets_version"), 1);
    const auto fetched = service.metadata("targets");
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->encoded, first.targets.dump());
    EXPECT_EQ(fetched->document, first.targets);
    EXPECT_TRUE(std::filesystem::exists(service.directory() / "1.root.json"));
    EXPECT_TRUE(std::filesystem::exists(service.directory() / "1.targets.json"));
    EXPECT_TRUE(std::filesystem::exists(service.directory() / "1.snapshot.json"));
    EXPECT_TRUE(std::filesystem::exists(service.directory() / "1.timestamp.json"));

    json below_threshold = targets_document(2, {&keys.target_a, &keys.target_a});
    json below_snapshot = snapshot_document(2, below_threshold, keys.snapshot);
    json below_timestamp = timestamp_document(2, below_snapshot, keys.timestamp);
    const auto threshold_failure =
        service.publish(below_threshold, below_snapshot, below_timestamp);
    EXPECT_FALSE(threshold_failure);
    EXPECT_NE(threshold_failure.error().message.find("signature threshold"), std::string::npos);

    const auto rollback = service.publish(first.targets, first.snapshot, first.timestamp);
    EXPECT_FALSE(rollback);
    EXPECT_EQ(rollback.error().code, "conflict");

    const Bundle expired =
        bundle(2, keys.targets(), keys.snapshot, keys.timestamp, "2000-01-01T00:00:00Z");
    const auto freeze = service.publish(expired.targets, expired.snapshot, expired.timestamp);
    EXPECT_FALSE(freeze);
    EXPECT_NE(freeze.error().message.find("expired"), std::string::npos);

    const Bundle second = bundle(2, keys.targets(), keys.snapshot, keys.timestamp);
    json mixed_snapshot = second.snapshot;
    mixed_snapshot["signed"]["meta"]["targets.json"] = binding(first.targets);
    mixed_snapshot = signed_document(mixed_snapshot.at("signed"), {&keys.snapshot});
    const json mixed_timestamp = timestamp_document(2, mixed_snapshot, keys.timestamp);
    const auto mixed = service.publish(second.targets, mixed_snapshot, mixed_timestamp);
    EXPECT_FALSE(mixed);
    EXPECT_NE(mixed.error().message.find("does not match"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(service.directory() / "2.targets.json"));
}

TEST(TransparencyTuf, RootRotationReplacesCompromisedRoleKeys) {
    TemporaryRoot temporary;
    Keys old_keys;
    TestKey new_root_a{7};
    TestKey new_root_b{39};
    TestKey new_root_c{71};
    TestKey new_target_a{103};
    TestKey new_target_b{135};
    const std::vector<const TestKey*> new_roots = {&new_root_a, &new_root_b, &new_root_c};
    const std::vector<const TestKey*> new_targets = {&new_target_a, &new_target_b};
    lake::transparency::Service service(temporary.path(), "instance-recovery");
    const json first_root = root_document(1,
                                          old_keys.roots(),
                                          old_keys.targets(),
                                          old_keys.snapshot,
                                          old_keys.timestamp,
                                          old_keys.root_threshold());
    ASSERT_TRUE(service.initialize(first_root));
    const Bundle first = bundle(1, old_keys.targets(), old_keys.snapshot, old_keys.timestamp);
    ASSERT_TRUE(service.publish(first.targets, first.snapshot, first.timestamp));

    const json unsigned_second_root =
        root_document(2, new_roots, new_targets, old_keys.snapshot, old_keys.timestamp, {});
    json old_only = signed_document(unsigned_second_root.at("signed"), old_keys.root_threshold());
    const auto missing_new_threshold = service.rotate_root(old_only);
    EXPECT_FALSE(missing_new_threshold);
    EXPECT_NE(missing_new_threshold.error().message.find("own root threshold"), std::string::npos);

    json new_only = signed_document(unsigned_second_root.at("signed"), {&new_root_a, &new_root_b});
    const auto missing_old_threshold = service.rotate_root(new_only);
    EXPECT_FALSE(missing_old_threshold);
    EXPECT_NE(missing_old_threshold.error().message.find("trusted root threshold"),
              std::string::npos);

    std::vector<const TestKey*> both_thresholds = old_keys.root_threshold();
    both_thresholds.push_back(&new_root_a);
    both_thresholds.push_back(&new_root_b);
    const json second_root = signed_document(unsigned_second_root.at("signed"), both_thresholds);
    const auto rotated = service.rotate_root(second_root);
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated->version, 2U);

    const Bundle compromised = bundle(2, old_keys.targets(), old_keys.snapshot, old_keys.timestamp);
    const auto old_key_publication =
        service.publish(compromised.targets, compromised.snapshot, compromised.timestamp);
    EXPECT_FALSE(old_key_publication);
    EXPECT_NE(old_key_publication.error().message.find("signature threshold"), std::string::npos);

    const Bundle recovered = bundle(2, new_targets, old_keys.snapshot, old_keys.timestamp);
    const auto new_key_publication =
        service.publish(recovered.targets, recovered.snapshot, recovered.timestamp);
    ASSERT_TRUE(new_key_publication) << new_key_publication.error().message;
}

TEST(TransparencyRegistry, CapabilitiesServeMetadataAndRecordAudit) {
    TemporaryRoot temporary;
    Keys keys;
    lake::transparency::Service service(temporary.path(), "registry-instance");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "registry-instance", air_gapped_configuration());
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.transparency = &service;
    context.checkpoints = &checkpoints;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "transparency-test-instance-kek";
    const json root = root_document(
        1, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold());
    const auto initialized = registry.call("initialize_tuf", {{"root", root}}, context);
    ASSERT_TRUE(initialized.ok) << initialized.error.message;
    const Bundle first = bundle(1, keys.targets(), keys.snapshot, keys.timestamp);
    context.request_id.clear();
    const auto published = registry.call(
        "publish_tuf_metadata",
        {{"targets", first.targets}, {"snapshot", first.snapshot}, {"timestamp", first.timestamp}},
        context);
    ASSERT_TRUE(published.ok) << published.error.message;
    context.request_id.clear();
    const auto configuration =
        registry.call("transparency_log_configuration", nlohmann::json::object(), context);
    ASSERT_TRUE(configuration.ok) << configuration.error.message;
    EXPECT_EQ(configuration.value.at("mode"), "air-gapped");
    context.request_id.clear();
    const auto checkpoint =
        registry.call("publish_transparency_checkpoint", nlohmann::json::object(), context);
    ASSERT_TRUE(checkpoint.ok) << checkpoint.error.message;
    EXPECT_EQ(checkpoint.value.at("sequence"), 1);
    context.request_id.clear();
    const auto fetched = registry.call("tuf_metadata", {{"role", "timestamp"}}, context);
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    EXPECT_EQ(fetched.value.at("encoded"), first.timestamp.dump());

    const auto events = store.list_audit_events({.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.kind == "transparency" && event.draft.action == "publish_tuf_metadata" &&
               event.draft.object == "transparency/log";
    }));
    EXPECT_TRUE(lake::audit::verify(store, *events));
}

TEST(PostgresTransparency, CapabilitiesPersistSignedAuditOutcomes) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporaryRoot temporary;
    Keys keys;
    lake::transparency::Service service(temporary.path(), "postgres-instance");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "postgres-instance", air_gapped_configuration());
    auto store = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = store->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = store.get();
    context.transparency = &service;
    context.checkpoints = &checkpoints;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    context.request_id = "postgres-transparency-initialize";
    const json root = root_document(
        1, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold());
    const auto initialized = registry.call("initialize_tuf", {{"root", root}}, context);
    ASSERT_TRUE(initialized.ok) << initialized.error.message;
    const Bundle first = bundle(1, keys.targets(), keys.snapshot, keys.timestamp);
    context.request_id = "postgres-transparency-publish";
    const auto published = registry.call(
        "publish_tuf_metadata",
        {{"targets", first.targets}, {"snapshot", first.snapshot}, {"timestamp", first.timestamp}},
        context);
    ASSERT_TRUE(published.ok) << published.error.message;
    const json second_root = root_document(
        2, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold());
    context.request_id = "postgres-transparency-rotate";
    const auto rotated = registry.call("rotate_tuf_root", {{"root", second_root}}, context);
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    context.request_id = "postgres-transparency-read";
    const auto fetched = registry.call("tuf_metadata", {{"role", "root"}}, context);
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    context.request_id = "postgres-transparency-configuration";
    const auto configuration =
        registry.call("transparency_log_configuration", nlohmann::json::object(), context);
    ASSERT_TRUE(configuration.ok) << configuration.error.message;
    context.request_id = "postgres-transparency-checkpoint";
    const auto checkpoint =
        registry.call("publish_transparency_checkpoint", nlohmann::json::object(), context);
    ASSERT_TRUE(checkpoint.ok) << checkpoint.error.message;

    const auto events = store->list_audit_events({.limit = 1000000});
    ASSERT_TRUE(events) << events.error().message;
    const auto event = std::ranges::find_if(*events, [](const lake::audit::Event& candidate) {
        return candidate.draft.request_id == "postgres-transparency-read";
    });
    ASSERT_NE(event, events->end());
    EXPECT_EQ(event->draft.kind, "transparency");
    EXPECT_EQ(event->draft.outcome, "success");
    for (const std::string_view action : {"initialize_tuf",
                                          "publish_tuf_metadata",
                                          "rotate_tuf_root",
                                          "tuf_metadata",
                                          "transparency_log_configuration",
                                          "publish_transparency_checkpoint"}) {
        EXPECT_TRUE(std::ranges::any_of(*events, [action](const lake::audit::Event& candidate) {
            return candidate.draft.kind == "transparency" && candidate.draft.action == action;
        }));
    }
    EXPECT_TRUE(lake::audit::verify(*store, *events));
}
