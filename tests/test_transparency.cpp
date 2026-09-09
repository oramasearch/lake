#include "audit_test_support.hpp"
#include "postgres_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <liquid/attestation.hpp>
#include <liquid/runtime.hpp>
#include <liquid/transparency.hpp>

#include "http1/client.hpp"
#include "objects/records.hpp"

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/transparency/checkpoint.hpp"
#include "lake/transparency/log.hpp"
#include "lake/transparency/timestamps.hpp"
#include "lake/transparency/tuf.hpp"
#include "temporary_directory.hpp"

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

struct CertificateDeleter {
    void operator()(X509* certificate) const {
        X509_free(certificate);
    }
};

struct ResponseContextDeleter {
    void operator()(TS_RESP_CTX* context) const {
        TS_RESP_CTX_free(context);
    }
};

struct ResponseDeleter {
    void operator()(TS_RESP* response) const {
        TS_RESP_free(response);
    }
};

struct BioDeleter {
    void operator()(BIO* bio) const {
        BIO_free(bio);
    }
};

using Certificate = std::unique_ptr<X509, CertificateDeleter>;
using ResponseContext = std::unique_ptr<TS_RESP_CTX, ResponseContextDeleter>;
using Response = std::unique_ptr<TS_RESP, ResponseDeleter>;
using Bio = std::unique_ptr<BIO, BioDeleter>;

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

Pkey p256_key() {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* key = nullptr;
    if (context == nullptr || EVP_PKEY_keygen_init(context) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(context, &key) != 1) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    EVP_PKEY_CTX_free(context);
    return Pkey(key);
}

bool add_extension(X509* certificate, X509* issuer, int nid, const char* value) {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension =
        X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char*>(value));
    if (extension == nullptr) {
        return false;
    }
    const bool added = X509_add_ext(certificate, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return added;
}

Certificate certificate(EVP_PKEY* key,
                        X509* issuer,
                        EVP_PKEY* issuer_key,
                        long serial,
                        const char* common_name,
                        bool authority) {
    Certificate result(X509_new());
    if (!result || X509_set_version(result.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(result.get()), serial) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(result.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(result.get()), 400L * 24 * 60 * 60) ||
        X509_set_pubkey(result.get(), key) != 1) {
        return {};
    }
    X509_NAME* subject = X509_get_subject_name(result.get());
    if (X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(common_name),
                                   -1,
                                   -1,
                                   0) != 1 ||
        X509_set_issuer_name(result.get(),
                             issuer == nullptr ? subject : X509_get_subject_name(issuer)) != 1) {
        return {};
    }
    const bool extensions =
        authority
            ? add_extension(
                  result.get(), result.get(), NID_basic_constraints, "critical,CA:TRUE") &&
                  add_extension(
                      result.get(), result.get(), NID_key_usage, "critical,keyCertSign,cRLSign")
            : add_extension(result.get(), issuer, NID_basic_constraints, "critical,CA:FALSE") &&
                  add_extension(result.get(), issuer, NID_key_usage, "critical,digitalSignature") &&
                  add_extension(result.get(), issuer, NID_ext_key_usage, "critical,timeStamping");
    if (!extensions || X509_sign(result.get(), issuer_key, EVP_sha256()) <= 0) {
        return {};
    }
    return result;
}

class StubTsa {
public:
    StubTsa() {
        root_key_ = p256_key();
        signer_key_ = p256_key();
        root_ =
            certificate(root_key_.get(), nullptr, root_key_.get(), 1, "Lake test TSA root", true);
        signer_ =
            certificate(signer_key_.get(), root_.get(), root_key_.get(), 2, "Lake test TSA", false);
        Bio output(BIO_new(BIO_s_mem()));
        if (output && root_ && PEM_write_bio_X509(output.get(), root_.get()) == 1) {
            BUF_MEM* memory = nullptr;
            BIO_get_mem_ptr(output.get(), &memory);
            trust_pem_.assign(memory->data, memory->length);
        }
    }

    lake::transparency::TsaReply send(std::string_view endpoint, std::string_view request) {
        EXPECT_EQ(endpoint, "https://tsa.example.test/rfc3161");
        ++requests_;
        ResponseContext context(TS_RESP_CTX_new());
        ASN1_OBJECT* policy = OBJ_txt2obj("1.3.6.1.4.1.57264.1", 1);
        Bio input(BIO_new_mem_buf(request.data(), static_cast<int>(request.size())));
        if (!context || !policy || !input || !root_ || !signer_ ||
            TS_RESP_CTX_set_signer_cert(context.get(), signer_.get()) != 1 ||
            TS_RESP_CTX_set_signer_key(context.get(), signer_key_.get()) != 1 ||
            TS_RESP_CTX_set_signer_digest(context.get(), EVP_sha256()) != 1 ||
            TS_RESP_CTX_set_def_policy(context.get(), policy) != 1 ||
            TS_RESP_CTX_add_md(context.get(), EVP_sha256()) != 1) {
            ASN1_OBJECT_free(policy);
            return {500, {}, "could not configure stub TSA"};
        }
        ASN1_OBJECT_free(policy);
        Response response(TS_RESP_create_response(context.get(), input.get()));
        if (!response) {
            return {500, {}, "could not create stub TSA response"};
        }
        const int size = i2d_TS_RESP(response.get(), nullptr);
        if (size <= 0) {
            return {500, {}, "could not encode stub TSA response"};
        }
        std::string body(static_cast<std::size_t>(size), '\0');
        unsigned char* cursor = reinterpret_cast<unsigned char*>(body.data());
        if (i2d_TS_RESP(response.get(), &cursor) != size) {
            return {500, {}, "could not encode stub TSA response"};
        }
        return {200, std::move(body), {}};
    }

    const std::string& trust_pem() const {
        return trust_pem_;
    }

    int requests() const {
        return requests_;
    }

private:
    Pkey root_key_;
    Pkey signer_key_;
    Certificate root_;
    Certificate signer_;
    std::string trust_pem_;
    int requests_ = 0;
};

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
        path_ = lake::test::fixture_path("tuf-" + std::to_string(++next));
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

std::string base64url(std::span<const std::byte> value) {
    std::string output(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    EXPECT_GE(size, 0);
    output.resize(static_cast<std::size_t>(size));
    std::ranges::replace(output, '+', '-');
    std::ranges::replace(output, '/', '_');
    while (output.ends_with('=')) {
        output.pop_back();
    }
    return output;
}

std::string signed_attestation(std::string identity, unsigned char marker) {
    liquid::attestation statement{
        .subject = liquid::digest{liquid::algorithm::blake3, {}},
        .profile = "authorship/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = liquid::attestation_subject_kind::drip,
        .attributes = {{"author", std::move(identity)}},
    };
    statement.subject.bytes.front() = std::byte{marker};
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    statement.issuer = signer->identity();
    auto body = liquid::objects::encode_attestation_body(statement);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    statement.signature = std::move(*signature);
    auto encoded = liquid::encode_attestation(statement);
    EXPECT_TRUE(encoded) << encoded.error().message;
    return base64url(*encoded);
}

std::vector<std::byte> decode_base64url(std::string value) {
    for (char& character : value) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::byte> output;
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    for (const char character : value) {
        const std::size_t position = alphabet.find(character);
        if (position == std::string_view::npos) {
            break;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(std::byte{static_cast<unsigned char>((accumulator >> bits) & 0xffU)});
        }
    }
    return output;
}

std::vector<std::byte> decode_hex(std::string_view value) {
    const auto nibble = [](char character) {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned char>(character - '0');
        }
        return static_cast<unsigned char>(character - 'a' + 10);
    };
    std::vector<std::byte> output;
    output.reserve(value.size() / 2);
    for (std::size_t index = 0; index + 1 < value.size(); index += 2) {
        output.push_back(std::byte{
            static_cast<unsigned char>((nibble(value[index]) << 4U) | nibble(value[index + 1]))});
    }
    return output;
}

liquid::transparency::hash decode_hash(std::string_view value) {
    const std::vector<std::byte> decoded = decode_hex(value);
    liquid::transparency::hash output{};
    std::ranges::copy(decoded, output.begin());
    return output;
}

struct OfflineProofBundle {
    liquid::actor identity;
    liquid::transparency::log_configuration configuration;
    liquid::transparency::checkpoint checkpoint;
    std::vector<liquid::transparency::tile> tiles;
    liquid::transparency::inclusion_proof inclusion;
};

OfflineProofBundle decode_proof_bundle(const json& bundle) {
    const json& record = bundle.at("checkpoint");
    const json& body = record.at("checkpoint");
    liquid::transparency::checkpoint checkpoint;
    checkpoint.sequence = record.at("sequence").get<std::uint64_t>();
    checkpoint.tree_size = body.at("tree_size").get<std::uint64_t>();
    checkpoint.entry_tile_width = body.at("tile_width").get<std::uint64_t>();
    checkpoint.hash_tile_width = body.at("hash_tile_width").get<std::uint64_t>();
    checkpoint.root_hash = decode_hash(body.at("root_hash").get<std::string>());
    checkpoint.checkpoint_hash =
        decode_hash(record.at("checkpoint_hash_sha256").get<std::string>());
    checkpoint.signature.signer = record.at("signature").at("signer").get<std::string>();
    checkpoint.signature.bytes =
        decode_base64url(record.at("signature").at("signature").get<std::string>());
    const std::string canonical_body = body.dump();
    checkpoint.canonical_body.assign(
        reinterpret_cast<const std::byte*>(canonical_body.data()),
        reinterpret_cast<const std::byte*>(canonical_body.data() + canonical_body.size()));

    liquid::transparency::log_configuration configuration;
    configuration.witness_threshold =
        bundle.at("configuration").at("witness_threshold").get<std::uint64_t>();
    for (const json& witness : bundle.at("configuration").at("witnesses")) {
        const auto public_key = decode_hash(witness.at("public_key").get<std::string>());
        configuration.witnesses.push_back({witness.at("endpoint").get<std::string>(), public_key});
    }
    for (const json& signature : record.at("witness_signatures")) {
        checkpoint.witness_signatures.push_back(
            {signature.at("endpoint").get<std::string>(),
             decode_hash(signature.at("public_key").get<std::string>()),
             decode_hex(signature.at("signature").get<std::string>())});
    }

    const json& identity_json = bundle.at("identity");
    liquid::actor identity{identity_json.at("taap_id").get<std::string>(),
                           "ed25519",
                           {},
                           decode_base64url(identity_json.at("public_key").get<std::string>()),
                           {}};
    std::vector<liquid::transparency::tile> tiles;
    for (const json& item : bundle.at("tiles")) {
        liquid::transparency::tile tile;
        tile.level = item.at("level").get<std::uint32_t>();
        tile.index = item.at("index").get<std::uint64_t>();
        tile.width = item.contains("entries") ? item.at("entries").get<std::uint64_t>()
                                              : item.at("hashes").get<std::uint64_t>();
        tile.sha256 = decode_hash(item.at("sha256").get<std::string>());
        tile.bytes = decode_base64url(item.at("tile").get<std::string>());
        tiles.push_back(std::move(tile));
    }
    const json& inclusion_json = bundle.at("inclusion");
    liquid::transparency::inclusion_proof inclusion;
    inclusion.leaf_index = inclusion_json.at("leaf_index").get<std::uint64_t>();
    inclusion.tree_size = inclusion_json.at("tree_size").get<std::uint64_t>();
    inclusion.root_hash = decode_hash(inclusion_json.at("root_hash").get<std::string>());
    inclusion.bound_checkpoint = checkpoint;
    for (const json& item : inclusion_json.at("audit_path")) {
        inclusion.audit_path.push_back(decode_hash(item.get<std::string>()));
    }
    return {std::move(identity),
            std::move(configuration),
            std::move(checkpoint),
            std::move(tiles),
            std::move(inclusion)};
}

class OfflineProofSource final : public liquid::transparency::log_source {
public:
    explicit OfflineProofSource(OfflineProofBundle bundle) : bundle_(std::move(bundle)) {}

    [[nodiscard]] std::string name() const override {
        return "exported lake proof bundle";
    }

    [[nodiscard]] liquid::runtime::task<liquid::actor> identity() override {
        ++bundle_reads;
        co_return bundle_.identity;
    }

    [[nodiscard]] liquid::runtime::task<liquid::transparency::log_configuration>
    configuration() override {
        ++bundle_reads;
        co_return bundle_.configuration;
    }

    [[nodiscard]] liquid::runtime::task<liquid::transparency::checkpoint>
    current_checkpoint() override {
        ++bundle_reads;
        co_return bundle_.checkpoint;
    }

    [[nodiscard]] liquid::runtime::task<liquid::transparency::tile>
    get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) override {
        ++bundle_reads;
        const auto found = std::ranges::find_if(bundle_.tiles, [=](const auto& tile) {
            return tile.level == level && tile.index == index && tile.width == width;
        });
        if (found == bundle_.tiles.end()) {
            co_return std::unexpected(liquid::error{
                liquid::errc::not_found, "tile is absent from offline bundle", name()});
        }
        co_return *found;
    }

    [[nodiscard]] liquid::runtime::task<liquid::transparency::inclusion_proof>
    get_inclusion_proof(std::uint64_t leaf_index, std::uint64_t tree_size) override {
        ++bundle_reads;
        if (bundle_.inclusion.leaf_index != leaf_index ||
            bundle_.inclusion.tree_size != tree_size) {
            co_return std::unexpected(liquid::error{
                liquid::errc::not_found, "proof is absent from offline bundle", name()});
        }
        co_return bundle_.inclusion;
    }

    [[nodiscard]] liquid::runtime::task<liquid::transparency::consistency_proof>
    get_consistency_proof(std::uint64_t, std::uint64_t) override {
        ++bundle_reads;
        co_return std::unexpected(liquid::error{
            liquid::errc::not_found, "consistency proof is absent from offline bundle", name()});
    }

    OfflineProofBundle bundle_;
    std::size_t bundle_reads = 0;
};

} // namespace

TEST(TransparencyLog, BulkTilesDetectTamperingAndScanOneHundredThousandEntries) {
    TemporaryRoot temporary;
    lake::transparency::TiledLog log(temporary.path(), "monitor-instance");
    const std::string monitored_identity = "taap:1:ed25519:monitored";
    const std::string monitored = signed_attestation(monitored_identity, 1);
    const std::string unrelated = signed_attestation("taap:1:ed25519:other", 2);
    constexpr std::size_t entry_count = 100000;
    constexpr std::size_t batch_size = 1000;
    const auto growth_started = std::chrono::steady_clock::now();
    for (std::size_t first = 0; first < entry_count; first += batch_size) {
        std::vector<std::string> batch;
        batch.reserve(batch_size);
        for (std::size_t index = first; index < first + batch_size; ++index) {
            batch.push_back(index % 1000 == 0 ? monitored : unrelated);
        }
        const auto appended = log.append(batch);
        ASSERT_TRUE(appended) << appended.error().message;
    }
    const double growth_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - growth_started)
            .count();
    const auto state = log.state();
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state->tree_size, entry_count);

    const auto first_tile = log.tile(0, lake::transparency::transparency_tile_width);
    ASSERT_TRUE(first_tile) << first_tile.error().message;
    EXPECT_EQ(first_tile->width, lake::transparency::transparency_tile_width);
    EXPECT_FALSE(first_tile->encoded.empty());
    const std::size_t final_width = entry_count % lake::transparency::transparency_tile_width;
    const auto final_tile =
        log.tile(entry_count / lake::transparency::transparency_tile_width, final_width);
    ASSERT_TRUE(final_tile) << final_tile.error().message;
    const auto full_hash_tile = log.hash_tile(1, 0, lake::transparency::transparency_tile_width);
    ASSERT_TRUE(full_hash_tile) << full_hash_tile.error().message;
    EXPECT_EQ(full_hash_tile->level, 1U);

    const auto scan = log.scan_identity(monitored_identity, state->tree_size, state->root_hash);
    ASSERT_TRUE(scan) << scan.error().message;
    EXPECT_TRUE(scan->duration_ms < growth_ms);
    EXPECT_EQ(scan->entries_scanned, entry_count);
    EXPECT_EQ(scan->tiles_scanned, 98U);
    ASSERT_EQ(scan->matches.size(), 100U);
    EXPECT_EQ(scan->matches.front().index, 0U);
    EXPECT_EQ(scan->matches.back().index, 99000U);
    std::cout << "100000-entry transparency growth_ms=" << growth_ms
              << " scan_ms=" << scan->duration_ms << '\n';

    const std::filesystem::path tile_path =
        log.directory() / "tiles" / "entries" / "0" / "1024.tile";
    std::fstream tile_file(tile_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(tile_file);
    tile_file.seekg(-1, std::ios::end);
    char byte = 0;
    tile_file.read(&byte, 1);
    byte ^= 1;
    tile_file.seekp(-1, std::ios::end);
    tile_file.write(&byte, 1);
    tile_file.close();
    const auto tampered = log.scan_identity(monitored_identity, state->tree_size, state->root_hash);
    EXPECT_FALSE(tampered);
    EXPECT_EQ(tampered.error().code, "unrecoverable");
}

TEST(TransparencyLog, PublishesParentTilesAndBuildsCompactProofs) {
    TemporaryRoot temporary;
    lake::transparency::TiledLog log(temporary.path(), "proof-instance");
    const std::string first = signed_attestation("taap:1:ed25519:proof-first", 21);
    const std::string second = signed_attestation("taap:1:ed25519:proof-second", 22);
    const std::vector<std::string> entries = {
        first,
        second,
        first,
        second,
        first,
        second,
        first,
    };
    const auto old_append =
        log.append(std::vector<std::string>(entries.begin(), entries.begin() + 3));
    ASSERT_TRUE(old_append) << old_append.error().message;
    EXPECT_EQ(old_append->hash_tiles_published, 1U);
    const auto old_state = log.state();
    ASSERT_TRUE(old_state) << old_state.error().message;
    const auto old_parent_tile = log.hash_tile(1, 0, 1);
    ASSERT_TRUE(old_parent_tile) << old_parent_tile.error().message;

    const auto new_append =
        log.append(std::vector<std::string>(entries.begin() + 3, entries.end()));
    ASSERT_TRUE(new_append) << new_append.error().message;
    EXPECT_EQ(new_append->hash_tiles_published, 2U);
    const auto new_state = log.state();
    ASSERT_TRUE(new_state) << new_state.error().message;
    ASSERT_EQ(new_state->tree_size, 7U);
    const auto parent_tile = log.hash_tile(1, 0, 3);
    ASSERT_TRUE(parent_tile) << parent_tile.error().message;
    EXPECT_EQ(parent_tile->width, 3U);
    const auto grandparent_tile = log.hash_tile(2, 0, 1);
    ASSERT_TRUE(grandparent_tile) << grandparent_tile.error().message;

    for (std::uint64_t leaf_index = 0; leaf_index < new_state->tree_size; ++leaf_index) {
        const auto proof = log.inclusion_proof(leaf_index, new_state->tree_size);
        ASSERT_TRUE(proof) << proof.error().message;
        EXPECT_EQ(proof->root_hash, new_state->root_hash);
        EXPECT_FALSE(proof->audit_path.empty());
    }
    const auto historical = log.inclusion_proof(2, old_state->tree_size);
    ASSERT_TRUE(historical) << historical.error().message;
    EXPECT_EQ(historical->root_hash, old_state->root_hash);
    const auto consistency = log.consistency_proof(old_state->tree_size, new_state->tree_size);
    ASSERT_TRUE(consistency) << consistency.error().message;
    EXPECT_EQ(consistency->old_root_hash, old_state->root_hash);
    EXPECT_EQ(consistency->new_root_hash, new_state->root_hash);
    EXPECT_EQ(consistency->audit_path.size(), 4U);
    EXPECT_FALSE(log.inclusion_proof(new_state->tree_size, new_state->tree_size));
    EXPECT_FALSE(log.consistency_proof(new_state->tree_size, old_state->tree_size));

    std::filesystem::remove_all(log.directory() / "tiles" / "hashes");
    const auto rebuilt = log.hash_tile(1, 0, 3);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(rebuilt->sha256, parent_tile->sha256);
    const auto rebuilt_historical = log.hash_tile(1, 0, 1);
    ASSERT_TRUE(rebuilt_historical) << rebuilt_historical.error().message;
    EXPECT_EQ(rebuilt_historical->sha256, old_parent_tile->sha256);
    const auto rebuilt_historical_proof = log.inclusion_proof(2, old_state->tree_size);
    ASSERT_TRUE(rebuilt_historical_proof) << rebuilt_historical_proof.error().message;
    EXPECT_EQ(rebuilt_historical_proof->root_hash, old_state->root_hash);
}

TEST(TransparencyLog, ConcurrentWritersPreserveEveryEntry) {
    TemporaryRoot temporary;
    lake::transparency::TiledLog first_log(temporary.path(), "concurrent-instance");
    lake::transparency::TiledLog second_log(temporary.path(), "concurrent-instance");
    const std::string identity = "taap:1:ed25519:concurrent";
    const std::string attestation = signed_attestation(identity, 3);
    const std::vector<std::string> first_entries(700, attestation);
    const std::vector<std::string> second_entries(700, attestation);

    auto first_append =
        std::async(std::launch::async, [&]() { return first_log.append(first_entries); });
    auto second_append =
        std::async(std::launch::async, [&]() { return second_log.append(second_entries); });
    const auto first_result = first_append.get();
    const auto second_result = second_append.get();
    ASSERT_TRUE(first_result) << first_result.error().message;
    ASSERT_TRUE(second_result) << second_result.error().message;

    const auto state = first_log.state();
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state->tree_size, 1400U);
    const auto scan = first_log.scan_identity(identity, state->tree_size, state->root_hash);
    ASSERT_TRUE(scan) << scan.error().message;
    EXPECT_EQ(scan->entries_scanned, 1400U);
    EXPECT_EQ(scan->matches.size(), 1400U);
}

TEST(TransparencyLog, TwoRegionsConvergeOnlyFromACommonPrefix) {
    TemporaryRoot temporary;
    lake::transparency::TiledLog source(temporary.path(), "replication-source");
    lake::transparency::TiledLog target(temporary.path(), "replication-target");
    const std::string attestation = signed_attestation("taap:1:ed25519:replication-subject", 18);
    std::vector<std::string> initial(1100, attestation);
    const auto source_initial = source.append(initial);
    ASSERT_TRUE(source_initial) << source_initial.error().message;
    const auto first_batch = source.replication_batch(0);
    ASSERT_TRUE(first_batch) << first_batch.error().message;
    lake::transparency::TiledLog tampered_target(temporary.path(), "replication-tampered");
    auto tampered = *first_batch;
    tampered.head.root_hash.front() = tampered.head.root_hash.front() == '0' ? '1' : '0';
    const auto rejected = tampered_target.apply_replication_batch(tampered);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "unrecoverable");
    const auto empty = tampered_target.state();
    ASSERT_TRUE(empty) << empty.error().message;
    EXPECT_EQ(empty->tree_size, 0U);

    const auto target_initial = target.apply_replication_batch(*first_batch);
    ASSERT_TRUE(target_initial) << target_initial.error().message;
    EXPECT_EQ(target_initial->tree_size, source_initial->tree_size);
    EXPECT_EQ(target_initial->root_hash, source_initial->root_hash);
    const auto retried = target.apply_replication_batch(*first_batch);
    ASSERT_TRUE(retried) << retried.error().message;
    EXPECT_EQ(retried->tiles_published, 0U);

    lake::transparency::TiledLog divergent_target(temporary.path(), "replication-divergent");
    const auto divergent_head =
        divergent_target.append({signed_attestation("taap:1:ed25519:replication-divergent", 20)});
    ASSERT_TRUE(divergent_head) << divergent_head.error().message;
    const auto divergent_rejected = divergent_target.apply_replication_batch(*first_batch);
    EXPECT_FALSE(divergent_rejected);
    EXPECT_EQ(divergent_rejected.error().code, "unrecoverable");
    const auto divergent_unchanged = divergent_target.state();
    ASSERT_TRUE(divergent_unchanged) << divergent_unchanged.error().message;
    EXPECT_EQ(divergent_unchanged->root_hash, divergent_head->root_hash);

    std::vector<std::string> suffix(17, attestation);
    const auto source_head = source.append(suffix);
    ASSERT_TRUE(source_head) << source_head.error().message;
    const auto second_batch = source.replication_batch(target_initial->tree_size);
    ASSERT_TRUE(second_batch) << second_batch.error().message;
    const auto target_head = target.apply_replication_batch(*second_batch);
    ASSERT_TRUE(target_head) << target_head.error().message;
    EXPECT_EQ(target_head->tree_size, source_head->tree_size);
    EXPECT_EQ(target_head->root_hash, source_head->root_hash);

    const auto unchanged = target.state();
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_EQ(unchanged->root_hash, source_head->root_hash);
}

TEST(TransparencyRegistry, ReplicationCapabilityConvergesTwoInstancesAndRecordsAudit) {
    TemporaryRoot temporary;
    lake::transparency::TiledLog source_log(temporary.path(), "registry-replication-source");
    lake::transparency::TiledLog target_log(temporary.path(), "registry-replication-target");
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext source_context;
    source_context.store = &store;
    source_context.node_name = "registry-replication-source";
    source_context.transparency_log = &source_log;
    source_context.authorized = true;
    source_context.operator_credential = true;
    source_context.acting_identity = "admin";
    source_context.instance_kek = "registry-replication-kek";
    lake::api::ApiContext target_context = source_context;
    target_context.node_name = "registry-replication-target";
    target_context.transparency_log = &target_log;
    target_context.replication_requester =
        [&](const std::string&, std::uint64_t after) -> std::expected<json, lake::api::ApiError> {
        source_context.request_id.clear();
        const auto response = registry.call(
            "export_transparency_replication_batch", {{"after_tree_size", after}}, source_context);
        if (!response.ok) {
            return std::unexpected(response.error);
        }
        return response.value;
    };
    const std::string attestation = signed_attestation("taap:1:ed25519:registry-replication", 19);
    ASSERT_TRUE(source_log.append(std::vector<std::string>(1025, attestation)));
    const auto replicated = registry.call(
        "replicate_transparency_log", {{"source_url", "https://source.example"}}, target_context);
    ASSERT_TRUE(replicated.ok) << replicated.error.message;
    EXPECT_TRUE(replicated.value.at("caught_up"));
    EXPECT_EQ(replicated.value.at("entries_applied"), 1025);
    const auto source_state = source_log.state();
    const auto target_state = target_log.state();
    ASSERT_TRUE(source_state);
    ASSERT_TRUE(target_state);
    EXPECT_EQ(target_state->tree_size, source_state->tree_size);
    EXPECT_EQ(target_state->root_hash, source_state->root_hash);

    const auto events = lake::test::audit_events(store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.kind == "replication" &&
               event.draft.action == "replicate_transparency_log";
    }));
    EXPECT_TRUE(lake::audit::verify(store, *events));
}

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
    json refreshed_body = checkpoint_body();
    refreshed_body["metadata_version"] = 2;
    const auto refreshed = service.publish(refreshed_body, checkpoint_signer(), {});
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_EQ(refreshed->at("sequence"), 2);
    const auto current = service.current();
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(*current, *refreshed);
    const auto selected = service.at_tree_size(12);
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(*selected, *refreshed);
    const auto missing = service.at_tree_size(11);
    EXPECT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "not_found");
    EXPECT_FALSE(std::filesystem::exists(service.directory() / "1.ots.json"));
}

TEST(TransparencyCheckpoint, IssuedAtUsesSigningClockAndRefusesRollbackAfterRestart) {
    TemporaryRoot temporary;
    auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1800000000}};
    const auto clock = [&now] { return now; };
    lake::transparency::CheckpointService service(
        temporary.path(), "clock", air_gapped_configuration(), clock);
    json signed_body;
    std::size_t calls = 0;
    const auto signer = [&](const json& body) -> lake::transparency::Result<json> {
        signed_body = body;
        ++calls;
        return checkpoint_signer()(body);
    };
    json supplied_body = checkpoint_body();
    supplied_body["issued_at"] = 1;
    const auto first = service.publish(supplied_body, signer, {});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(signed_body.at("issued_at"), 1800000000U);
    EXPECT_EQ(first->at("checkpoint"), signed_body);
    EXPECT_EQ(first->at("checkpoint_hash_sha256"), sha256(signed_body.dump()));

    const auto equal = service.publish(checkpoint_body(), signer, {});
    ASSERT_TRUE(equal) << equal.error().message;
    EXPECT_EQ(equal->at("checkpoint").at("issued_at"), signed_body.at("issued_at"));
    now -= std::chrono::seconds{1};
    lake::transparency::CheckpointService reopened(
        temporary.path(), "clock", air_gapped_configuration(), clock);
    const auto refused = reopened.publish(checkpoint_body(), signer, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "conflict");
    EXPECT_EQ(refused.error().message, "checkpoint issued_at would move backwards");
    EXPECT_EQ(calls, 2U);
    ASSERT_TRUE(reopened.current());
    EXPECT_EQ(*reopened.current(), *equal);
    now += std::chrono::seconds{2};
    const auto later = reopened.publish(checkpoint_body(), signer, {});
    ASSERT_TRUE(later) << later.error().message;
    EXPECT_EQ(later->at("sequence"), 3);
    EXPECT_EQ(later->at("checkpoint").at("issued_at"), 1800000001U);
}

TEST(TransparencyCheckpoint, ConcurrentPublishersSerializeSequenceAndSigningTime) {
    TemporaryRoot temporary;
    std::atomic<std::uint64_t> seconds{1800000000};
    const auto clock = [&seconds] {
        return std::chrono::system_clock::time_point{std::chrono::seconds{seconds.fetch_add(1)}};
    };
    lake::transparency::CheckpointService first(
        temporary.path(), "concurrent", air_gapped_configuration(), clock);
    lake::transparency::CheckpointService second(
        temporary.path(), "concurrent", air_gapped_configuration(), clock);
    struct Publication {
        lake::transparency::Result<json> result;
    };
    std::vector<std::future<Publication>> publications;
    for (std::size_t index = 0; index < 16; ++index) {
        auto* service = index % 2 == 0 ? &first : &second;
        publications.push_back(std::async(std::launch::async, [service] {
            return Publication{service->publish(checkpoint_body(), checkpoint_signer(), {})};
        }));
    }
    for (std::size_t index = 0; index < publications.size(); ++index) {
        const auto result = publications[index].get().result;
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(result->at("checkpoint").at("issued_at").get<std::uint64_t>(),
                  1799999999U + result->at("sequence").get<std::uint64_t>());
    }
    const auto current = first.current();
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(current->at("sequence"), 16U);
}

TEST(TransparencyCheckpoint, ReplicationRejectsTimeRollbackAndPreservesLegacyRecords) {
    TemporaryRoot temporary;
    lake::transparency::CheckpointService source(
        temporary.path(), "source", air_gapped_configuration());
    lake::transparency::CheckpointService replica(
        temporary.path(), "replica", air_gapped_configuration());
    const auto first = source.publish(checkpoint_body(), checkpoint_signer(), {});
    ASSERT_TRUE(first) << first.error().message;
    auto malformed = *first;
    malformed["checkpoint"]["issued_at"] = "1800000000";
    malformed["checkpoint_hash_sha256"] = sha256(malformed.at("checkpoint").dump());
    const auto invalid = replica.adopt(malformed);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, "invalid_input");
    auto legacy = *first;
    legacy["checkpoint"].erase("issued_at");
    legacy["checkpoint_hash_sha256"] = sha256(legacy.at("checkpoint").dump());
    ASSERT_TRUE(replica.adopt(legacy));
    const auto second = source.publish(checkpoint_body(), checkpoint_signer(), {});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(replica.adopt(*second));
    auto backwards = *second;
    backwards["sequence"] = 3U;
    backwards["checkpoint"]["issued_at"] =
        second->at("checkpoint").at("issued_at").get<std::uint64_t>() - 1;
    backwards["checkpoint_hash_sha256"] = sha256(backwards.at("checkpoint").dump());
    const auto refused = replica.adopt(backwards);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "conflict");
    backwards["checkpoint"].erase("issued_at");
    backwards["checkpoint_hash_sha256"] = sha256(backwards.at("checkpoint").dump());
    EXPECT_FALSE(replica.adopt(backwards));
    ASSERT_TRUE(replica.current());
    EXPECT_EQ(*replica.current(), *second);
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
    EXPECT_TRUE(anchored_checkpoint->at("checkpoint").contains("issued_at"));
    EXPECT_TRUE(std::filesystem::exists(anchored_service.directory() / "1.ots.json"));
}

TEST(TransparencyTimestamp, CreatesAndRenewsRfc4998EvidenceFromStubTsa) {
    TemporaryRoot temporary;
    StubTsa tsa;
    ASSERT_FALSE(tsa.trust_pem().empty());
    lake::transparency::TimestampService service(temporary.path(), "timestamp-instance");
    lake::transparency::TimestampPolicy policy;
    policy.renewal_interval = std::chrono::seconds{1};
    policy.renew_before_expiry = std::chrono::seconds::zero();
    const auto sender = [&tsa](std::string_view endpoint, std::string_view request) {
        return tsa.send(endpoint, request);
    };
    const std::string checkpoint_digest = sha256("signed checkpoint");
    const std::string signature = "checkpoint signature value";
    const auto created =
        service.timestamp(checkpoint_digest,
                          lake::control::identity_crypto::base64url_encode(signature),
                          "https://tsa.example.test/rfc3161",
                          tsa.trust_pem(),
                          policy,
                          sender);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->checkpoint_digest, checkpoint_digest);
    EXPECT_EQ(created->signature_digest, sha256(signature));
    EXPECT_EQ(created->digest_algorithm, "sha256");
    ASSERT_EQ(created->archive_timestamps.size(), 1U);
    EXPECT_FALSE(created->evidence_record.empty());
    EXPECT_TRUE(std::filesystem::exists(service.directory() / (checkpoint_digest + ".ers")));

    const auto due =
        service.due(std::chrono::system_clock::now() + std::chrono::seconds{2}, policy);
    ASSERT_TRUE(due) << due.error().message;
    EXPECT_EQ(*due, std::vector<std::string>({checkpoint_digest}));

    const auto renewed = service.renew(
        checkpoint_digest, "https://tsa.example.test/rfc3161", tsa.trust_pem(), policy, sender);
    ASSERT_TRUE(renewed) << renewed.error().message;
    ASSERT_EQ(renewed->archive_timestamps.size(), 2U);
    EXPECT_EQ(renewed->archive_timestamps.front().token_sha256,
              created->archive_timestamps.front().token_sha256);
    EXPECT_NE(renewed->archive_timestamps.back().token_sha256,
              created->archive_timestamps.front().token_sha256);
    EXPECT_EQ(tsa.requests(), 2);

    const auto fetched = service.evidence(checkpoint_digest, policy);
    ASSERT_TRUE(fetched) << fetched.error().message;
    EXPECT_EQ(fetched->evidence_record, renewed->evidence_record);
    EXPECT_EQ(fetched->archive_timestamps.size(), 2U);

    const std::filesystem::path evidence_path = service.directory() / (checkpoint_digest + ".ers");
    std::fstream evidence_file(evidence_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(evidence_file);
    evidence_file.seekg(-1, std::ios::end);
    char corrupt = '\0';
    evidence_file.read(&corrupt, 1);
    ASSERT_TRUE(evidence_file);
    corrupt ^= 0x01;
    evidence_file.seekp(-1, std::ios::end);
    evidence_file.write(&corrupt, 1);
    evidence_file.close();
    const auto tampered = service.evidence(checkpoint_digest, policy);
    EXPECT_FALSE(tampered);
}

TEST(TransparencyTimestamp, RejectsAnUntrustedTsaSigner) {
    TemporaryRoot temporary;
    StubTsa tsa;
    StubTsa unrelated_authority;
    lake::transparency::TimestampService service(temporary.path(), "untrusted-tsa");
    const auto rejected = service.timestamp(
        sha256("untrusted checkpoint"),
        lake::control::identity_crypto::base64url_encode("untrusted checkpoint signature"),
        "https://tsa.example.test/rfc3161",
        unrelated_authority.trust_pem(),
        {},
        [&tsa](std::string_view endpoint, std::string_view request) {
            return tsa.send(endpoint, request);
        });

    EXPECT_FALSE(rejected);
    EXPECT_FALSE(std::filesystem::exists(service.directory()));
}

TEST(TransparencyTimestampRegistry, CapabilitiesTimestampRenewReadAndAudit) {
    TemporaryRoot temporary;
    StubTsa tsa;
    lake::transparency::TimestampService service(temporary.path(), "timestamp-registry");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "timestamp-registry", air_gapped_configuration());
    const auto checkpoint = checkpoints.publish(
        checkpoint_body(),
        [](const json&) -> lake::transparency::Result<json> {
            return json{
                {"signer", "taap:1:ed25519:lake"},
                {"signature",
                 lake::control::identity_crypto::base64url_encode("registry checkpoint signature")},
            };
        },
        {});
    ASSERT_TRUE(checkpoint) << checkpoint.error().message;
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.timestamping = &service;
    context.checkpoints = &checkpoints;
    context.tsa_endpoint = "https://tsa.example.test/rfc3161";
    context.tsa_trust_pem = tsa.trust_pem();
    context.tsa_sender = [&tsa](std::string_view endpoint, std::string_view request) {
        return tsa.send(endpoint, request);
    };
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "timestamp-registry-instance-kek";
    const std::string checkpoint_digest = checkpoint->at("checkpoint_hash_sha256");
    const auto timestamped =
        registry.call("timestamp_checkpoint", nlohmann::json::object(), context);
    ASSERT_TRUE(timestamped.ok) << timestamped.error.message;
    EXPECT_EQ(timestamped.value.at("archive_timestamps").size(), 1U);
    EXPECT_EQ(timestamped.value.at("signature_digest"), sha256("registry checkpoint signature"));
    context.request_id.clear();
    const auto renewed = registry.call(
        "renew_timestamp_evidence", {{"checkpoint_digest", checkpoint_digest}}, context);
    ASSERT_TRUE(renewed.ok) << renewed.error.message;
    EXPECT_EQ(renewed.value.at("archive_timestamps").size(), 2U);
    context.request_id.clear();
    const auto fetched =
        registry.call("timestamp_evidence", {{"checkpoint_digest", checkpoint_digest}}, context);
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    EXPECT_EQ(fetched.value.at("evidence_record"), renewed.value.at("evidence_record"));

    const auto events = lake::test::audit_events(store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    for (const std::string_view action :
         {"timestamp_checkpoint", "renew_timestamp_evidence", "timestamp_evidence"}) {
        EXPECT_TRUE(std::ranges::any_of(*events, [action](const lake::audit::Event& event) {
            return event.draft.kind == "transparency" && event.draft.action == action &&
                   event.draft.object == "transparency/log";
        }));
    }
    EXPECT_TRUE(lake::audit::verify(store, *events));
}

TEST(TransparencyTimestamp, OwnerLoopRenewsDueEvidenceThroughRegistry) {
    TemporaryRoot temporary;
    StubTsa tsa;
    const std::string checkpoint_digest = sha256("owner loop checkpoint");
    lake::transparency::TimestampPolicy policy;
    policy.renew_before_expiry = std::chrono::days{500};
    const auto sender = [&tsa](std::string_view endpoint, std::string_view request) {
        return tsa.send(endpoint, request);
    };
    lake::transparency::TimestampService initial(temporary.path(), "owner-loop");
    const auto created = initial.timestamp(
        checkpoint_digest,
        lake::control::identity_crypto::base64url_encode("owner loop checkpoint signature"),
        "https://tsa.example.test/rfc3161",
        tsa.trust_pem(),
        policy,
        sender);
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_EQ(tsa.requests(), 1);

    auto store = std::make_shared<lake::control::InMemoryStore>();
    lake::api::ServerOptions options;
    options.store = store;
    options.store_root = temporary.path();
    options.node_name = "owner-loop";
    options.instance_kek = "owner-loop-instance-kek";
    options.log_configuration = air_gapped_configuration();
    options.tsa_endpoint = "https://tsa.example.test/rfc3161";
    options.tsa_trust_pem = tsa.trust_pem();
    options.timestamp_policy = policy;
    options.tsa_sender = sender;
    lake::api::ApiServer server(std::move(options));

    server.drain();

    EXPECT_EQ(tsa.requests(), 2);
    const auto renewed = initial.evidence(checkpoint_digest, policy);
    ASSERT_TRUE(renewed) << renewed.error().message;
    EXPECT_EQ(renewed->archive_timestamps.size(), 2U);
    const auto events = lake::test::audit_events(*store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.action == "renew_timestamp_evidence" &&
               event.draft.method == "scheduled_timestamp_renewal" &&
               event.draft.outcome == "success";
    }));
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
    lake::transparency::TiledLog log(temporary.path(), "registry-instance");
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.transparency = &service;
    context.checkpoints = &checkpoints;
    context.transparency_log = &log;
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
    const std::string monitored_identity = "taap:1:ed25519:registry-monitor";
    const std::string attestation = signed_attestation(monitored_identity, 3);
    context.request_id.clear();
    const auto appended = registry.call(
        "append_transparency_entries", {{"attestations", {attestation, attestation}}}, context);
    ASSERT_TRUE(appended.ok) << appended.error.message;
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
    EXPECT_EQ(checkpoint.value.at("checkpoint").at("tree_size"), 2);
    context.request_id.clear();
    const auto tile = registry.call("transparency_tile", {{"index", 0}, {"width", 2}}, context);
    ASSERT_TRUE(tile.ok) << tile.error.message;
    EXPECT_EQ(tile.value.at("entries"), 2);
    context.request_id.clear();
    const auto hash_tile =
        registry.call("transparency_tile", {{"index", 0}, {"level", 1}, {"width", 1}}, context);
    ASSERT_TRUE(hash_tile.ok) << hash_tile.error.message;
    EXPECT_EQ(hash_tile.value.at("hashes"), 1);
    context.request_id.clear();
    const auto inclusion = registry.call(
        "transparency_inclusion_proof", {{"leaf_index", 0}, {"tree_size", 2}}, context);
    ASSERT_TRUE(inclusion.ok) << inclusion.error.message;
    EXPECT_EQ(inclusion.value.at("checkpoint").at("sequence"), 1);
    EXPECT_EQ(inclusion.value.at("audit_path").size(), 1U);
    context.request_id.clear();
    const auto consistency = registry.call(
        "transparency_consistency_proof", {{"old_tree_size", 1}, {"new_tree_size", 2}}, context);
    ASSERT_TRUE(consistency.ok) << consistency.error.message;
    EXPECT_EQ(consistency.value.at("checkpoint").at("sequence"), 1);
    EXPECT_EQ(consistency.value.at("audit_path").size(), 1U);
    context.request_id.clear();
    const auto scan =
        registry.call("transparency_identity_scan", {{"identity", monitored_identity}}, context);
    ASSERT_TRUE(scan.ok) << scan.error.message;
    EXPECT_EQ(scan.value.at("matches").size(), 2U);
    context.request_id.clear();
    const auto fetched = registry.call("tuf_metadata", {{"role", "timestamp"}}, context);
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    EXPECT_EQ(fetched.value.at("encoded"), first.timestamp.dump());

    const auto events = lake::test::audit_events(store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.kind == "transparency" && event.draft.action == "publish_tuf_metadata" &&
               event.draft.object == "transparency/log";
    }));
    EXPECT_TRUE(lake::audit::verify(store, *events));
}

TEST(TransparencyScenario, ExportedBundleVerifiesOfflineAndSplitViewIsDetected) {
    TemporaryRoot temporary;
    Keys keys;
    lake::transparency::Service service(temporary.path(), "offline-instance");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "offline-instance", air_gapped_configuration());
    lake::transparency::TiledLog log(temporary.path(), "offline-instance");
    lake::control::InMemoryStore store;
    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    context.store = &store;
    context.transparency = &service;
    context.checkpoints = &checkpoints;
    context.transparency_log = &log;
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.instance_kek = "offline-proof-instance-kek";
    const json root = root_document(
        1, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold());
    const auto initialized = registry.call("initialize_tuf", {{"root", root}}, context);
    ASSERT_TRUE(initialized.ok) << initialized.error.message;
    const Bundle metadata = bundle(1, keys.targets(), keys.snapshot, keys.timestamp);
    context.request_id.clear();
    const auto metadata_published = registry.call("publish_tuf_metadata",
                                                  {{"targets", metadata.targets},
                                                   {"snapshot", metadata.snapshot},
                                                   {"timestamp", metadata.timestamp}},
                                                  context);
    ASSERT_TRUE(metadata_published.ok) << metadata_published.error.message;
    context.request_id.clear();
    const std::string attestation = signed_attestation("taap:1:ed25519:offline-proof-subject", 7);
    const auto appended = registry.call(
        "append_transparency_entries", {{"attestations", {attestation, attestation}}}, context);
    ASSERT_TRUE(appended.ok) << appended.error.message;
    context.request_id.clear();
    const auto published =
        registry.call("publish_transparency_checkpoint", json::object(), context);
    ASSERT_TRUE(published.ok) << published.error.message;
    context.request_id.clear();
    const auto exported = registry.call("transparency_proof_bundle", {{"leaf_index", 0}}, context);
    ASSERT_TRUE(exported.ok) << exported.error.message;
    EXPECT_EQ(exported.value.at("version"), "lake_transparency_proof_bundle/1");

    OfflineProofSource offline(decode_proof_bundle(exported.value));
    liquid::transparency::log_client verifier(offline);
    auto verifier_repository = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "offline-verifier"));
    ASSERT_TRUE(verifier_repository) << verifier_repository.error().message;
    const auto verified = liquid::runtime::block_on(
        verifier.verify_inclusion(*verifier_repository,
                                  decode_base64url(attestation),
                                  liquid::transparency::bootstrap_policy::pinned));
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_FALSE(verified->bootstrap_warning);
    EXPECT_GT(offline.bundle_reads, 0U);

    auto gossip_repository =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "gossip"));
    ASSERT_TRUE(gossip_repository) << gossip_repository.error().message;
    const liquid::transparency::checkpoint first_witness_view = offline.bundle_.checkpoint;
    json second_record = exported.value.at("checkpoint");
    json second_body = second_record.at("checkpoint");
    std::string second_root = second_body.at("root_hash").get<std::string>();
    second_root.front() = second_root.front() == '0' ? '1' : '0';
    second_body["root_hash"] = second_root;
    const auto second_signature = lake::audit::sign_transparency_checkpoint(
        store, second_body, {.instance_kek = context.instance_kek});
    ASSERT_TRUE(second_signature) << second_signature.error().message;
    second_record["checkpoint"] = second_body;
    second_record["checkpoint_hash_sha256"] = sha256(second_body.dump());
    second_record["signature"] = *second_signature;
    json second_export = exported.value;
    second_export["checkpoint"] = std::move(second_record);
    const liquid::transparency::checkpoint second_witness_view =
        decode_proof_bundle(second_export).checkpoint;
    const liquid::transparency::log_trust trust{offline.bundle_.identity,
                                                offline.bundle_.configuration};
    ASSERT_TRUE(first_witness_view.verify(trust));
    ASSERT_TRUE(second_witness_view.verify(trust));
    const auto trusted_first = liquid::runtime::block_on(
        liquid::transparency::trust_checkpoint(*gossip_repository,
                                               "two-witness checkpoint gossip",
                                               first_witness_view,
                                               liquid::transparency::bootstrap_policy::pinned));
    ASSERT_TRUE(trusted_first) << trusted_first.error().message;
    const auto detected_split = liquid::runtime::block_on(
        liquid::transparency::trust_checkpoint(*gossip_repository,
                                               "two-witness checkpoint gossip",
                                               second_witness_view,
                                               liquid::transparency::bootstrap_policy::pinned));
    ASSERT_FALSE(detected_split);
    EXPECT_EQ(detected_split.error().code, liquid::errc::conflict);
}

TEST(PostgresTransparency, LiveCheckpointCarriesAuthenticatedMonotonicTime) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporaryRoot temporary;
    Keys keys;
    lake::transparency::Service metadata(temporary.path(), "issued-at-live");
    const auto initialized = metadata.initialize(root_document(
        1, keys.roots(), keys.targets(), keys.snapshot, keys.timestamp, keys.root_threshold()));
    ASSERT_TRUE(initialized) << initialized.error().message;
    const Bundle tuf = bundle(1, keys.targets(), keys.snapshot, keys.timestamp);
    const auto provisioned = metadata.publish(tuf.targets, tuf.snapshot, tuf.timestamp);
    ASSERT_TRUE(provisioned) << provisioned.error().message;
    lake::transparency::TiledLog log(temporary.path(), "issued-at-live");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "issued-at-live", air_gapped_configuration());
    const std::string kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    std::shared_ptr<lake::control::Store> store =
        lake::control::make_postgres_store(database_uri, kek);
    const auto ready = store->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    std::uint64_t previous_time = 0;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const auto appended = log.append({signed_attestation("taap:1:ed25519:live", sequence)});
        ASSERT_TRUE(appended) << appended.error().message;
        lake::api::ServerOptions options;
        options.port = 0;
        options.admin_token = "issued-at-test-token";
        options.store = store;
        options.store_root = temporary.path();
        options.node_name = "issued-at-live";
        options.instance_kek = kek;
        options.log_configuration = air_gapped_configuration();
        lake::api::ApiServer server(std::move(options));
        ASSERT_TRUE(server.start()) << server.reason();
        const auto before =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
                .time_since_epoch()
                .count();
        const auto published_http = liquid::runtime::block_on(
            liquid::http1::request({.url = "http://127.0.0.1:" + std::to_string(server.port()),
                                    .method = "POST",
                                    .suffix = "/v1/publish_transparency_checkpoint",
                                    .headers = {{"Authorization", "Bearer issued-at-test-token"},
                                                {"Content-Type", "application/json"}},
                                    .body = {std::byte{'{'}, std::byte{'}'}}}));
        ASSERT_TRUE(published_http) << published_http.error().message;
        ASSERT_EQ(published_http->status, 200U);
        const auto after =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
                .time_since_epoch()
                .count();
        liquid::transparency::http_log_source source("http://127.0.0.1:" +
                                                     std::to_string(server.port()));
        const auto identity = liquid::runtime::block_on(source.identity());
        ASSERT_TRUE(identity) << identity.error().message;
        const auto configuration = liquid::runtime::block_on(source.configuration());
        ASSERT_TRUE(configuration) << configuration.error().message;
        const liquid::transparency::log_trust trust{*identity, *configuration};
        liquid::transparency::log_client client(source);
        const auto verified = liquid::runtime::block_on(client.current_checkpoint(trust));
        ASSERT_TRUE(verified) << verified.error().message;
        const auto body =
            json::parse(reinterpret_cast<const char*>(verified->canonical_body.data()),
                        reinterpret_cast<const char*>(verified->canonical_body.data() +
                                                      verified->canonical_body.size()));
        ASSERT_TRUE(body.at("issued_at").is_number_unsigned());
        const auto issued_at = body.at("issued_at").get<std::uint64_t>();
        EXPECT_GE(issued_at, before);
        EXPECT_LE(issued_at, after);
        EXPECT_GE(issued_at, previous_time);
        EXPECT_EQ(verified->sequence, sequence);
        previous_time = issued_at;

        auto tampered = *verified;
        auto altered_body = body;
        altered_body["issued_at"] = issued_at + 1;
        const std::string encoded = altered_body.dump();
        tampered.canonical_body.assign(
            reinterpret_cast<const std::byte*>(encoded.data()),
            reinterpret_cast<const std::byte*>(encoded.data() + encoded.size()));
        tampered.checkpoint_hash = decode_hash(sha256(encoded));
        const auto rejected = tampered.verify(trust);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, liquid::errc::unauthorized);
        const auto published = checkpoints.current();
        ASSERT_TRUE(published) << published.error().message;
        std::cout << "Published checkpoint: " << published->dump() << '\n';
        std::cout << "Existing Liquid signature verification: accepted; issued_at=" << issued_at
                  << "; sequence=" << sequence << "; tampered issued_at: refused\n";
        server.stop();
    }
}

TEST(PostgresTransparency, CapabilitiesPersistSignedAuditOutcomes) {
    const char* database_uri = lake::test::postgres_uri();
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporaryRoot temporary;
    Keys keys;
    StubTsa tsa;
    lake::transparency::Service service(temporary.path(), "postgres-instance");
    lake::transparency::CheckpointService checkpoints(
        temporary.path(), "postgres-instance", air_gapped_configuration());
    lake::transparency::TiledLog log(temporary.path(), "postgres-instance");
    lake::transparency::TimestampService timestamping(temporary.path(), "postgres-instance");
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
    context.transparency_log = &log;
    context.timestamping = &timestamping;
    context.tsa_endpoint = "https://tsa.example.test/rfc3161";
    context.tsa_trust_pem = tsa.trust_pem();
    context.tsa_sender = [&tsa](std::string_view endpoint, std::string_view request) {
        return tsa.send(endpoint, request);
    };
    context.authorized = true;
    context.operator_credential = true;
    context.acting_identity = "admin";
    context.node_name = "postgres-instance";
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
    const std::string monitored_identity = "taap:1:ed25519:postgres-monitor";
    const std::string attestation = signed_attestation(monitored_identity, 4);
    context.request_id = "postgres-transparency-append";
    const auto appended = registry.call(
        "append_transparency_entries", {{"attestations", {attestation, attestation}}}, context);
    ASSERT_TRUE(appended.ok) << appended.error.message;
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
    context.request_id = "postgres-transparency-tile";
    const auto tile = registry.call("transparency_tile", {{"index", 0}, {"width", 2}}, context);
    ASSERT_TRUE(tile.ok) << tile.error.message;
    context.request_id = "postgres-transparency-hash-tile";
    const auto hash_tile =
        registry.call("transparency_tile", {{"index", 0}, {"level", 1}, {"width", 1}}, context);
    ASSERT_TRUE(hash_tile.ok) << hash_tile.error.message;
    context.request_id = "postgres-transparency-inclusion";
    const auto inclusion = registry.call(
        "transparency_inclusion_proof", {{"leaf_index", 0}, {"tree_size", 2}}, context);
    ASSERT_TRUE(inclusion.ok) << inclusion.error.message;
    context.request_id = "postgres-transparency-consistency";
    const auto consistency = registry.call(
        "transparency_consistency_proof", {{"old_tree_size", 1}, {"new_tree_size", 2}}, context);
    ASSERT_TRUE(consistency.ok) << consistency.error.message;
    context.request_id = "postgres-transparency-scan";
    const auto scan =
        registry.call("transparency_identity_scan", {{"identity", monitored_identity}}, context);
    ASSERT_TRUE(scan.ok) << scan.error.message;
    EXPECT_EQ(scan.value.at("matches").size(), 2U);
    const std::string checkpoint_digest = checkpoint.value.at("checkpoint_hash_sha256");
    context.request_id = "postgres-transparency-timestamp";
    const auto timestamped =
        registry.call("timestamp_checkpoint", nlohmann::json::object(), context);
    ASSERT_TRUE(timestamped.ok) << timestamped.error.message;
    context.request_id = "postgres-transparency-renew";
    const auto renewed = registry.call(
        "renew_timestamp_evidence", {{"checkpoint_digest", checkpoint_digest}}, context);
    ASSERT_TRUE(renewed.ok) << renewed.error.message;
    context.request_id = "postgres-transparency-evidence";
    const auto timestamp_evidence =
        registry.call("timestamp_evidence", {{"checkpoint_digest", checkpoint_digest}}, context);
    ASSERT_TRUE(timestamp_evidence.ok) << timestamp_evidence.error.message;
    EXPECT_EQ(timestamp_evidence.value.at("archive_timestamps").size(), 2U);

    lake::transparency::CheckpointService replica_checkpoints(
        temporary.path(), "postgres-replica", air_gapped_configuration());
    lake::transparency::TiledLog replica_log(temporary.path(), "postgres-replica");
    lake::api::ApiContext replica_context = context;
    replica_context.node_name = "postgres-replica";
    replica_context.checkpoints = &replica_checkpoints;
    replica_context.transparency_log = &replica_log;
    replica_context.replication_requester =
        [&](const std::string&, std::uint64_t after) -> std::expected<json, lake::api::ApiError> {
        context.request_id = "postgres-transparency-replication-export";
        const auto response = registry.call(
            "export_transparency_replication_batch", {{"after_tree_size", after}}, context);
        if (!response.ok) {
            return std::unexpected(response.error);
        }
        return response.value;
    };
    replica_context.request_id = "postgres-transparency-replication-apply";
    const auto replicated = registry.call("replicate_transparency_log",
                                          {{"source_url", "https://postgres-source.example"}},
                                          replica_context);
    ASSERT_TRUE(replicated.ok) << replicated.error.message;
    EXPECT_TRUE(replicated.value.at("caught_up"));
    EXPECT_EQ(replicated.value.at("checkpoint_sequence"), 1);
    const auto replica_checkpoint = replica_checkpoints.current();
    ASSERT_TRUE(replica_checkpoint) << replica_checkpoint.error().message;
    EXPECT_EQ(replica_checkpoint->at("checkpoint_hash_sha256"),
              checkpoint.value.at("checkpoint_hash_sha256"));

    const auto events = lake::test::audit_events(*store, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    const auto read_events =
        lake::test::audit_events(*store, {.request_id = "postgres-transparency-read"});
    ASSERT_TRUE(read_events) << read_events.error().message;
    ASSERT_EQ(read_events->size(), 1U);
    EXPECT_EQ(read_events->front().draft.kind, "transparency");
    EXPECT_EQ(read_events->front().draft.outcome, "success");
    for (const std::string_view action : {"initialize_tuf",
                                          "publish_tuf_metadata",
                                          "append_transparency_entries",
                                          "rotate_tuf_root",
                                          "tuf_metadata",
                                          "transparency_log_configuration",
                                          "publish_transparency_checkpoint",
                                          "transparency_tile",
                                          "transparency_inclusion_proof",
                                          "transparency_consistency_proof",
                                          "transparency_identity_scan",
                                          "timestamp_checkpoint",
                                          "renew_timestamp_evidence",
                                          "timestamp_evidence"}) {
        EXPECT_TRUE(std::ranges::any_of(*events, [action](const lake::audit::Event& candidate) {
            return candidate.draft.kind == "transparency" && candidate.draft.action == action;
        }));
    }
    for (const std::string_view action :
         {"export_transparency_replication_batch", "replicate_transparency_log"}) {
        EXPECT_TRUE(std::ranges::any_of(*events, [action](const lake::audit::Event& candidate) {
            return candidate.draft.kind == "replication" && candidate.draft.action == action;
        }));
    }
    EXPECT_TRUE(lake::audit::verify(*store, *events));
}
