#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <openssl/hmac.h>

#include <liquid/liquid.hpp>

#include "../modules/liquid/src/objects/records.hpp"

#include "lake/api/capabilities.hpp"
#include "lake/api/forge.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/server.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/apps.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/telemetry/metrics.hpp"
#include "temporary_directory.hpp"

using nlohmann::json;
using namespace lake::api;

extern char** environ;

namespace {

struct Surface {
    Registry registry;
    lake::control::InMemoryStore store;
    ApiContext ctx;
    Surface() {
        register_core_capabilities(registry, "0.0.1");
        ctx.store = &store;
        ctx.node_name = "test";
        ctx.instance_kek = "test-instance-kek";
        ctx.authorized = true;
        ctx.acting_identity = "admin";
        ctx.operator_credential = true;
    }
    ApiResult call(const std::string& name, json input = json::object()) {
        ctx.request_id.clear();
        return registry.call(name, input, ctx);
    }
};

std::vector<std::byte> bytes(const std::string& value);

struct RepositoryFixtureStore final : SyncStore {
    std::map<std::string, RepositoryPortableData> imported;
    std::uint64_t unique_bytes = 123;
    bool restored = false;
    bool shared_folder_moved = false;
    std::optional<std::string> blob_contents;
    std::string commit_message;
    std::vector<RepositoryWriteOperation> committed_operations;
    std::string resolved_conflict;
    std::string conflict_resolution;
    std::vector<std::byte> conflict_blob;
    SyncStoreResult<std::vector<std::string>>
    find_missing(const std::string&, const std::vector<std::string>&) override {
        return std::vector<std::string>{};
    }

    SyncStoreResult<void>
    write(const std::string&, const std::string&, std::vector<std::byte>, std::uint64_t) override {
        return {};
    }

    SyncStoreResult<std::vector<std::byte>> read(const std::string&, const std::string&) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::uint64_t> unique_stored_bytes(const std::string&) override {
        return unique_bytes;
    }

    SyncStoreResult<std::vector<RepositoryContentRef>>
    repository_refs(const std::string&, const std::string&) override {
        return std::vector<RepositoryContentRef>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<RepositoryTreeEntry>>
    repository_tree(const std::string&, const std::string&, const std::string&) override {
        const std::string shared_path = shared_folder_moved ? "renamed" : "shared";
        return std::vector<RepositoryTreeEntry>{
            {"readme.md", "blob", "blob-1", 0644, "11111111111111111111111111111111"},
            {"MODEL_CARD.md", "blob", "blob-card", 0644, "44444444444444444444444444444444"},
            {"weights.bin", "blob", "blob-weights", 0644, "55555555555555555555555555555555"},
            {shared_path, "tree", "", 0755, "22222222222222222222222222222222"},
            {shared_path + "/readme.md",
             "blob",
             "blob-2",
             0644,
             "33333333333333333333333333333333"}};
    }

    SyncStoreResult<RepositoryFile> repository_file(const std::string&,
                                                    const std::string&,
                                                    const std::string& revision,
                                                    const std::string& path) override {
        if (path == "MODEL_CARD.md") {
            return RepositoryFile{path, "blob-card", "# Fixture model", 15};
        }
        if (path == "weights.bin") {
            return RepositoryFile{path, "blob-weights", "fixture weights", 15};
        }
        if (revision == "change-1") {
            return RepositoryFile{path, "blob-1", "old\nshared\n", 11, bytes("old\nshared\n")};
        }
        return RepositoryFile{path, "blob-2", "new\nshared\n", 11, bytes("new\nshared\n")};
    }

    SyncStoreResult<RepositoryFile> repository_blob(const std::string&,
                                                    const std::string&,
                                                    const std::string& path,
                                                    const std::string& blob) override {
        const std::string contents =
            blob_contents.value_or(blob == "blob-1" ? "old\nshared\n" : "new\nshared\n");
        return RepositoryFile{path, blob, contents, contents.size(), bytes(contents)};
    }

    SyncStoreResult<std::vector<RepositoryChange>> repository_log(const std::string&,
                                                                  const std::string&) override {
        return std::vector<RepositoryChange>{
            {"change-1", {}, "taap:1:ed25519:author", "initial", "2026-09-04T00:00:00Z"}};
    }

    SyncStoreResult<std::vector<RepositoryDiffEntry>>
    repository_diff(const std::string&, const std::string&, const std::string&) override {
        return std::vector<RepositoryDiffEntry>{
            {.path = "readme.md",
             .kind = "modified",
             .changes = {"modified"},
             .old_mode = 0644,
             .new_mode = 0644,
             .old_blob = "blob-1",
             .new_blob = "blob-2",
             .line_hunks = std::vector<RepositoryLineHunk>{{1, 1, 1, 1, {"old"}, {"hello"}}}}};
    }

    SyncStoreResult<RepositoryArtifactPage> repository_artifacts(
        const std::string&, const std::string&, const RepositoryArtifactFilter&) override {
        return RepositoryArtifactPage{
            .items =
                {{.id = "result-1",
                  .action = "action-1",
                  .outputs = {{"build/app", 0755, "blob-3"}},
                  .timing = {.sandbox_ms = 250, .exec_ms = 1000, .hash_ms = 250, .store_ms = 250},
                  .cache_status = "hit",
                  .worker = "worker-1",
                  .observed_at = "2026-09-04T00:00:00Z"}},
            .done = true};
    }

    SyncStoreResult<RepositoryArtifact> repository_artifact(const std::string&,
                                                            const std::string&,
                                                            const std::string& result_id) override {
        return RepositoryArtifact{
            .id = result_id,
            .action = "action-1",
            .outputs = {{"build/app", 0755, "blob-3"}},
            .timing = {.queued_ms = 1, .sandbox_ms = 2, .exec_ms = 3, .hash_ms = 4, .store_ms = 5},
            .cache_status = "hit",
            .worker = "worker-1",
            .observed_at = "2026-09-04T00:00:00Z",
            .attestations =
                {{"attestation-1",
                  "slsa_provenance/1",
                  "subject-1",
                  "taap:1:ed25519:builder",
                  true,
                  ""},
                 {"attestation-2", "sbom/1", "subject-1", "taap:1:ed25519:builder", true, ""}},
            .trace = RepositoryArtifactTrace{"trace-1", 12, 2, 1, 1, 1}};
    }

    SyncStoreResult<std::vector<RepositoryConflict>>
    repository_conflicts(const std::string&, const std::string&) override {
        return std::vector<RepositoryConflict>{
            {.id = "conflict-1",
             .base = "blob-base",
             .left = "blob-1",
             .right = "blob-2",
             .region = {.path = "readme.md", .bytes = RepositoryConflictByteRange{0, 3}},
             .kind = "suspicious-merge",
             .explanation = "Both sides changed the greeting.",
             .proposal = "blob-proposal",
             .reasons = {"The edits choose different words."},
             .version_blobs = {"blob-1", "blob-2"}}};
    }

    SyncStoreResult<RepositoryConflictResolution>
    resolve_repository_conflict(const std::string&,
                                const std::string&,
                                const std::string& conflict_id,
                                const std::string& resolution,
                                std::vector<std::byte> blob) override {
        resolved_conflict = conflict_id;
        conflict_resolution = resolution;
        conflict_blob = std::move(blob);
        return RepositoryConflictResolution{conflict_id, "blob-resolved", "change-resolved"};
    }

    SyncStoreResult<std::string>
    commit_repository_files(const std::string&,
                            const std::string&,
                            const std::string&,
                            const std::string& message,
                            std::vector<RepositoryWriteOperation> operations) override {
        commit_message = message;
        committed_operations = std::move(operations);
        return "drive-change";
    }

    SyncStoreResult<RepositoryChange> restore_repository_file(const std::string&,
                                                              const std::string&,
                                                              const std::string& change_id,
                                                              const std::string& path) override {
        restored = true;
        return RepositoryChange{"restored-change",
                                {change_id},
                                "taap:1:ed25519:author",
                                "Restore " + path,
                                "2026-09-04T01:00:00Z"};
    }

    SyncStoreResult<RepositoryPortableData>
    export_portable(const std::string&, const std::string&, bool include_git) override {
        RepositoryPortableData output;
        output.history = {{"refs", {{{"name", "main"}, {"change", "change-1"}}}},
                          {"changes",
                           {{{"id", "change-1"},
                             {"parents", json::array()},
                             {"author", "taap:1:ed25519:author"},
                             {"message", "initial"},
                             {"occurred_at", "2026-09-04T00:00:00Z"}}}}};
        output.artifacts = {{{"id", "result-1"}, {"action", "action-1"}}};
        output.git = {{"requested", include_git}};
        return output;
    }

    SyncStoreResult<void> import_portable(const std::string& customer_id,
                                          const std::string& repository_id,
                                          const RepositoryPortableData& data) override {
        imported[customer_id + "/" + repository_id] = data;
        return {};
    }

    SyncStoreResult<RepositoryGitImport>
    import_git(const std::string&, const std::string&, const std::string&) override {
        return RepositoryGitImport{
            .commits = 1, .blobs = 1, .commit_changes = {{"deadbeef", "change-1"}}};
    }

    SyncStoreResult<std::vector<std::pair<std::string, std::string>>>
    refs(const std::string&, const std::string&) override {
        return std::vector<std::pair<std::string, std::string>>{{"main", "change-1"}};
    }

    SyncStoreResult<std::vector<std::byte>>
    find_missing(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>> push(const std::string&,
                                                 const std::string&,
                                                 std::span<const std::byte>,
                                                 SyncPushAuthorizer authorize,
                                                 std::uint64_t) override {
        const auto authorized =
            authorize({.kind = SyncPushItemKind::Change, .change_id = "change-refused"}, {});
        if (!authorized) {
            return std::unexpected(authorized.error());
        }
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::vector<std::byte>>
    pull(const std::string&, const std::string&, std::span<const std::byte>) override {
        return std::vector<std::byte>{};
    }

    SyncStoreResult<std::uint64_t> upload_chunks(const std::string&,
                                                 const std::string&,
                                                 std::span<const std::byte>,
                                                 std::uint64_t) override {
        return 0;
    }

    SyncStoreResult<std::vector<std::byte>> read_chunk(const std::string&,
                                                       const std::string&,
                                                       const std::string&,
                                                       std::uint64_t,
                                                       std::uint64_t) override {
        return std::vector<std::byte>{};
    }
};

TEST(Metering, CapabilityReportsExactStorageAndIdempotentBuildCompute) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    surface.ctx.metering = {
        .currency = "USD", .storage_quota_bytes = 100, .compute_second_micros = 5};
    const ApiResult user =
        surface.call("create_user", {{"email", "meter@example.com"}, {"display_name", "Meter"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value.at("id")}, {"name", "meter"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value.at("id")}, {"name", "usage"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;

    const json input{{"customer_id", customer.value.at("id")}};
    const ApiResult first = surface.call("metering_usage", input);
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_FALSE(first.value.at("items").empty());
    EXPECT_EQ(first.value.at("items").back().at("stored_bytes"), 123);
    EXPECT_EQ(first.value.at("items").front().at("compute_seconds"), 2);
    EXPECT_EQ(first.value.at("cost_micros"), 10);
    EXPECT_TRUE(first.value.at("over_quota"));

    const ApiResult second = surface.call("metering_usage", input);
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("items").front().at("compute_seconds"), 2);
}

TEST(Metering, CustomerQuotaCapabilityOverridesTheProcessDefault) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    surface.ctx.metering.storage_quota_bytes = 100;
    const ApiResult user =
        surface.call("create_user", {{"email", "quota@example.com"}, {"display_name", "Quota"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value.at("id")}, {"name", "quota"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string customer_id = customer.value.at("id");
    const ApiResult configured =
        surface.call("set_storage_quota", {{"customer_id", customer_id}, {"quota_bytes", 10}});
    ASSERT_TRUE(configured.ok) << configured.error.message;
    EXPECT_EQ(configured.value.at("quota_bytes"), 10);
    const ApiResult usage = surface.call("metering_usage", {{"customer_id", customer_id}});
    ASSERT_TRUE(usage.ok) << usage.error.message;
    EXPECT_EQ(usage.value.at("quota_bytes"), 10);
    EXPECT_TRUE(usage.value.at("over_quota"));
}

TEST(Organizations, MemberMutationCapabilitiesUseTheRegistry) {
    Surface surface;
    const ApiResult first = surface.call("create_user",
                                         {{"taap_id", "taap:1:ed25519:first-owner"},
                                          {"email", "first-owner@example.com"},
                                          {"display_name", "First owner"}});
    const ApiResult second = surface.call("create_user",
                                          {{"taap_id", "taap:1:ed25519:second-owner"},
                                           {"email", "second-owner@example.com"},
                                           {"display_name", "Second owner"}});
    const ApiResult organization =
        surface.call("create_organization", {{"display_name", "Members"}});
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    ASSERT_TRUE(organization.ok);
    const auto create = [&](const ApiResult& user, const std::string& role) {
        return surface.call("create_membership",
                            {{"organization_id", organization.value.at("id")},
                             {"principal_type", "user"},
                             {"principal_id", user.value.at("id")},
                             {"principal_taap_id", user.value.at("taap_id")},
                             {"role", role},
                             {"attestation_digest", "sha512-256:membership"}});
    };
    const ApiResult first_membership = create(first, "owner");
    const ApiResult second_membership = create(second, "owner");
    ASSERT_TRUE(first_membership.ok) << first_membership.error.message;
    ASSERT_TRUE(second_membership.ok) << second_membership.error.message;
    const ApiResult changed = surface.call(
        "set_member_role", {{"id", first_membership.value.at("id")}, {"role", "admin"}});
    ASSERT_TRUE(changed.ok) << changed.error.message;
    EXPECT_EQ(changed.value.at("role"), "admin");
    const ApiResult removed =
        surface.call("remove_member", {{"id", first_membership.value.at("id")}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_TRUE(removed.value.at("removed"));
}

class TemporaryRepositoryRoot {
public:
    TemporaryRepositoryRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("web-ui-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRepositoryRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TemporarySyncRoot {
public:
    TemporarySyncRoot() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("sync-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporarySyncRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(const std::string& value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::string base64_bytes(std::span<const std::byte> input) {
    if (input.empty()) {
        return {};
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(input.data()),
                                     static_cast<int>(input.size()));
    output.resize(static_cast<std::size_t>(size));
    return output;
}

liquid::attestation sign_attestation(liquid::attestation value) {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    value.issuer = signer->identity();
    auto body = liquid::objects::encode_attestation_body(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    EXPECT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::encode_attestation_signature(*signed_value);
    EXPECT_TRUE(signature) << signature.error().message;
    value.signature = std::move(*signature);
    return value;
}

std::string share_token_hash(std::string_view token) {
    static constexpr std::string_view key = "lake-drive-share-link/1";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(token.data()),
         token.size(),
         digest,
         &size);
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string output;
    output.reserve(size * 2);
    for (unsigned index = 0; index < size; ++index) {
        output += alphabet[digest[index] >> 4U];
        output += alphabet[digest[index] & 0x0fU];
    }
    return output;
}

std::vector<std::byte> sync_bytes(std::string_view value) {
    std::vector<std::byte> output;
    output.reserve(value.size());
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

std::uint32_t test_u32_le(const std::array<std::byte, 49>& header, std::size_t offset) {
    return std::to_integer<std::uint32_t>(header[offset]) |
           std::to_integer<std::uint32_t>(header[offset + 1]) << 8U |
           std::to_integer<std::uint32_t>(header[offset + 2]) << 16U |
           std::to_integer<std::uint32_t>(header[offset + 3]) << 24U;
}

std::uint64_t unique_pack_raw_bytes(const std::filesystem::path& packs) {
    std::set<std::string> digests;
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(packs)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pack") {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        input.seekg(16);
        for (;;) {
            std::array<std::byte, 49> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
                std::string_view(reinterpret_cast<const char*>(header.data()), 6) != "LQFRAM") {
                break;
            }
            const std::string digest(reinterpret_cast<const char*>(header.data() + 16), 33);
            if (digests.insert(digest).second) {
                total += test_u32_le(header, 8);
            }
            input.seekg(test_u32_le(header, 12), std::ios::cur);
        }
    }
    return total;
}

std::vector<std::string> tree_fingerprint(const liquid::tree& tree) {
    std::vector<std::string> output;
    output.reserve(tree.entries.size());
    for (const auto& entry : tree.entries) {
        output.push_back(entry.name + ":" + liquid::to_string(entry.ref));
    }
    std::ranges::sort(output);
    return output;
}

const liquid::sync_ref_outcome* reference_outcome(const liquid::sync_report& report,
                                                  std::string_view name) {
    const auto outcome =
        std::ranges::find(report.references, name, &liquid::sync_ref_outcome::name);
    return outcome == report.references.end() ? nullptr : &*outcome;
}

bool send_socket_bytes(int socket_fd, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(socket_fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::size_t content_length_of(std::string_view header) {
    constexpr std::string_view name = "Content-Length:";
    const std::size_t start = header.find(name);
    if (start == std::string_view::npos) {
        return 0;
    }
    const std::size_t value_start = start + name.size();
    const std::size_t value_end = header.find("\r\n", value_start);
    return static_cast<std::size_t>(
        std::stoull(std::string(header.substr(value_start, value_end - value_start))));
}

std::string read_http_request(int socket_fd) {
    std::string request;
    char buffer[4096];
    std::size_t expected = 0;
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return request;
        }
        request.append(buffer, static_cast<std::size_t>(count));
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos && expected == 0) {
            expected = header_end + 4 + content_length_of(request.substr(0, header_end + 4));
        }
        if (expected != 0 && request.size() >= expected) {
            return request;
        }
    }
}

class RecordingProxy {
public:
    explicit RecordingProxy(unsigned short upstream_port) : upstream_port_(upstream_port) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ < 0 ||
            ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            return;
        }
        socklen_t size = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~RecordingProxy() {
        stop();
    }

    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (wake >= 0) {
            static_cast<void>(
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            ::close(wake);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

    [[nodiscard]] std::vector<std::byte> push_body() const {
        std::lock_guard lock(mutex_);
        return push_body_;
    }

private:
    void run() {
        while (!stopped_.load()) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            if (stopped_.load()) {
                ::close(client);
                break;
            }
            const std::string request = read_http_request(client);
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos &&
                request.substr(0, header_end).find("/sync/push ") != std::string::npos) {
                const auto* first =
                    reinterpret_cast<const std::byte*>(request.data() + header_end + 4);
                std::lock_guard lock(mutex_);
                push_body_.assign(first, first + request.size() - header_end - 4);
            }
            const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(upstream_port_);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (upstream >= 0 &&
                ::connect(upstream, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                send_socket_bytes(upstream, request)) {
                std::string response;
                char buffer[4096];
                for (;;) {
                    const ssize_t count = ::recv(upstream, buffer, sizeof(buffer), 0);
                    if (count <= 0) {
                        break;
                    }
                    response.append(buffer, static_cast<std::size_t>(count));
                }
                static_cast<void>(send_socket_bytes(client, response));
            }
            if (upstream >= 0) {
                ::close(upstream);
            }
            ::close(client);
        }
    }

    unsigned short upstream_port_{};
    int listener_{-1};
    unsigned short port_{};
    std::atomic<bool> stopped_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::byte> push_body_;
};

liquid::platform host_platform() {
#if defined(__APPLE__)
    return {"macos", "arm64"};
#elif defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

std::string
http_get(unsigned short port, std::string_view path, std::string_view authorization = {}) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }

    std::string request =
        "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    request += "\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string http_request(unsigned short port,
                         std::string_view method,
                         std::string_view path,
                         std::string_view body,
                         std::string_view content_type,
                         std::string_view cookie = {},
                         std::string_view authorization = {},
                         std::string_view host = "localhost") {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return {};
    }
    std::string request = std::string(method) + " " + std::string(path) +
                          " HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n";
    request += "Content-Type: " + std::string(content_type) + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!cookie.empty())
        request += "Cookie: " + std::string(cookie) + "\r\n";
    if (!authorization.empty())
        request += "Authorization: " + std::string(authorization) + "\r\n";
    request += "\r\n" + std::string(body);
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) {
            ::close(socket_fd);
            return {};
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::string response_header(std::string_view response, std::string_view name) {
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const std::size_t start = response.find(prefix);
    if (start == std::string_view::npos)
        return {};
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = response.find("\r\n", value_start);
    return std::string(response.substr(value_start, end - value_start));
}

json response_json(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return json();
    }
    return json::parse(response.substr(separator + 4), nullptr, false);
}

std::string response_body(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return {};
    }
    return std::string(response.substr(separator + 4));
}

std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

unsigned short unused_loopback_port() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return 0;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(socket_fd);
        return 0;
    }
    ::close(socket_fd);
    return ntohs(address.sin_port);
}

class LakeProcess {
public:
    LakeProcess(unsigned short port,
                const std::string& node,
                const std::string& database_uri,
                const std::filesystem::path& store_root) {
        std::vector<std::string> arguments{
            LAKE_TEST_SERVER_PATH,
            "--listen",
            "127.0.0.1:" + std::to_string(port),
            "--node",
            node,
            "--control-db",
            database_uri,
            "--store-root",
            store_root.string(),
            "--admin-token",
            "replication-admin",
            "--instance-kek",
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        };
        std::vector<char*> raw;
        raw.reserve(arguments.size() + 1);
        for (std::string& argument : arguments) {
            raw.push_back(argument.data());
        }
        raw.push_back(nullptr);
        if (::posix_spawn(&pid_, raw.front(), nullptr, nullptr, raw.data(), environ) != 0) {
            pid_ = -1;
            return;
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            const std::string ready = http_request(port, "GET", "/readyz", "", "application/json");
            if (ready.find(" 200 ") != std::string::npos) {
                ready_ = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~LakeProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            static_cast<void>(::waitpid(pid_, nullptr, 0));
        }
    }

    [[nodiscard]] bool ready() const {
        return ready_;
    }

private:
    pid_t pid_{-1};
    bool ready_ = false;
};

} // namespace

TEST(RegistryProtocols, OciClientPushesAndPullsTwoLayersByteIdentical) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "oci-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "registry-test-instance-kek";
    options.store_root = temporary.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer registry-admin";

    const std::string ping = http_request(server.port(), "GET", "/v2/", "", "application/json");
    ASSERT_NE(ping.find(" 200 "), std::string::npos) << ping;
    const std::string unauthorized = http_request(
        server.port(), "POST", "/v2/lake-fixture/blobs/uploads/", "", "application/octet-stream");
    EXPECT_NE(unauthorized.find(" 401 "), std::string::npos) << unauthorized;

    const auto push_blob = [&](const std::string& contents) {
        const std::string digest = "sha256:" + lake::control::identity_crypto::sha256_hex(contents);
        const std::string started = http_request(server.port(),
                                                 "POST",
                                                 "/v2/lake-fixture/blobs/uploads/",
                                                 "",
                                                 "application/octet-stream",
                                                 {},
                                                 authorization);
        EXPECT_NE(started.find(" 202 "), std::string::npos) << started;
        const std::string location = response_header(started, "Location");
        EXPECT_FALSE(location.empty()) << started;
        const std::string completed = http_request(server.port(),
                                                   "PUT",
                                                   location + "?digest=" + digest,
                                                   contents,
                                                   "application/octet-stream",
                                                   {},
                                                   authorization);
        EXPECT_NE(completed.find(" 201 "), std::string::npos) << completed;
        EXPECT_EQ(response_header(completed, "Docker-Content-Digest"), digest);
        return digest;
    };

    const std::string config = R"({"architecture":"arm64","os":"darwin"})";
    const std::string first_layer("lake layer one\0with binary", 26);
    const std::string second_layer = "lake layer two has different bytes\n";
    const std::string config_digest = push_blob(config);
    const std::string first_digest = push_blob(first_layer);
    const std::string second_digest = push_blob(second_layer);
    const json manifest{{"schemaVersion", 2},
                        {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
                        {"config",
                         {{"mediaType", "application/vnd.oci.image.config.v1+json"},
                          {"digest", config_digest},
                          {"size", config.size()}}},
                        {"layers",
                         {{{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                           {"digest", first_digest},
                           {"size", first_layer.size()}},
                          {{"mediaType", "application/vnd.oci.image.layer.v1.tar"},
                           {"digest", second_digest},
                           {"size", second_layer.size()}}}}};
    const std::string manifest_text = manifest.dump();
    const std::string published = http_request(server.port(),
                                               "PUT",
                                               "/v2/lake-fixture/manifests/latest",
                                               manifest_text,
                                               "application/vnd.oci.image.manifest.v1+json",
                                               {},
                                               authorization);
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;

    const std::string pulled_manifest = http_request(
        server.port(), "GET", "/v2/lake-fixture/manifests/latest", "", "application/json");
    EXPECT_EQ(response_body(pulled_manifest), manifest_text);
    for (const auto& [digest, expected] : std::array<std::pair<std::string, std::string>, 2>{
             std::pair{first_digest, first_layer}, std::pair{second_digest, second_layer}}) {
        const std::string pulled = http_request(server.port(),
                                                "GET",
                                                "/v2/lake-fixture/blobs/" + digest,
                                                "",
                                                "application/octet-stream");
        EXPECT_EQ(response_body(pulled), expected);
    }
    server.stop();
}

TEST(RegistryProtocols, NpmPublishesAndInstallsWithTheRealClient) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "npm-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "registry-test-instance-kek";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    if (std::system("command -v npm >/dev/null 2>&1") != 0) {
        const std::string tarball = "protocol-level npm fixture";
        const json metadata{{"name", "lake-issue-3-fixture"},
                            {"dist-tags", {{"latest", "1.0.0"}}},
                            {"versions",
                             {{"1.0.0",
                               {{"name", "lake-issue-3-fixture"},
                                {"version", "1.0.0"},
                                {"dist", {{"tarball", "placeholder"}}}}}}},
                            {"_attachments",
                             {{"lake-issue-3-fixture-1.0.0.tgz",
                               {{"content_type", "application/octet-stream"},
                                {"data", base64_bytes(bytes(tarball))},
                                {"length", tarball.size()}}}}}};
        const std::string published = http_request(server.port(),
                                                   "PUT",
                                                   "/lake-issue-3-fixture",
                                                   metadata.dump(),
                                                   "application/json",
                                                   {},
                                                   "Bearer registry-admin");
        ASSERT_NE(published.find(" 201 "), std::string::npos) << published;
        const std::string pulled = http_request(server.port(),
                                                "GET",
                                                "/lake-issue-3-fixture/-/"
                                                "lake-issue-3-fixture-1.0.0.tgz",
                                                "",
                                                "application/octet-stream");
        EXPECT_EQ(response_body(pulled), tarball);
        server.stop();
        return;
    }

    const std::filesystem::path package = temporary.path() / "package";
    const std::filesystem::path installed = temporary.path() / "installed";
    const std::filesystem::path cache = temporary.path() / "npm-cache";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(installed);
    {
        std::ofstream output(package / "package.json");
        output << R"({"name":"lake-issue-3-fixture","version":"1.0.0","main":"index.js"})";
    }
    {
        std::ofstream output(package / "index.js");
        output << "module.exports = 'published through lake';\n";
    }
    const std::string registry = "http://127.0.0.1:" + std::to_string(server.port());
    const std::filesystem::path user_config = temporary.path() / "npmrc";
    {
        std::ofstream output(user_config);
        output << "registry=" << registry << "/\n";
        output << "//127.0.0.1:" << server.port() << "/:_authToken=registry-admin\n";
    }
    const std::filesystem::path publish_log = temporary.path() / "npm-publish.log";
    const std::string publish_command = "npm publish '" + package.string() + "' --userconfig='" +
                                        user_config.string() + "' --cache='" + cache.string() +
                                        "' --ignore-scripts --loglevel=error >'" +
                                        publish_log.string() + "' 2>&1";
    ASSERT_EQ(std::system(publish_command.c_str()), 0) << read_binary_file(publish_log);
    const std::filesystem::path install_log = temporary.path() / "npm-install.log";
    const std::string install_command = "cd '" + installed.string() +
                                        "' && npm install lake-issue-3-fixture@1.0.0"
                                        " --userconfig='" +
                                        user_config.string() + "' --cache='" + cache.string() +
                                        "' --ignore-scripts --no-audit --no-fund --loglevel=error"
                                        " >'" +
                                        install_log.string() + "' 2>&1";
    ASSERT_EQ(std::system(install_command.c_str()), 0) << read_binary_file(install_log);
    EXPECT_EQ(read_binary_file(installed / "node_modules/lake-issue-3-fixture/index.js"),
              "module.exports = 'published through lake';\n");
    server.stop();
}

TEST(RegistryProtocols, PythonPackagePublishesAndInstallsWithPip) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "python-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "registry-test-instance-kek";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::filesystem::path wheel =
        temporary.path() / "lake_issue_3_fixture-1.0.0-py3-none-any.whl";
    const std::filesystem::path script = temporary.path() / "make_wheel.py";
    {
        std::ofstream output(script);
        output << R"py(import pathlib
import sys
import zipfile

wheel = pathlib.Path(sys.argv[1])
files = {
    "lake_issue_3_fixture/__init__.py": "value = 'published through lake'\n",
    "lake_issue_3_fixture-1.0.0.dist-info/METADATA": (
        "Metadata-Version: 2.1\nName: lake-issue-3-fixture\nVersion: 1.0.0\n"
    ),
    "lake_issue_3_fixture-1.0.0.dist-info/WHEEL": (
        "Wheel-Version: 1.0\nGenerator: lake-test\nRoot-Is-Purelib: true\nTag: py3-none-any\n"
    ),
    "lake_issue_3_fixture-1.0.0.dist-info/RECORD": "",
}
with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as archive:
    for name, contents in files.items():
        archive.writestr(name, contents)
)py";
    }
    const std::string wheel_command = "python3 '" + script.string() + "' '" + wheel.string() + "'";
    ASSERT_EQ(std::system(wheel_command.c_str()), 0);
    const std::string wheel_bytes = read_binary_file(wheel);
    const std::string boundary = "lake-issue-3-boundary";
    std::string publication;
    const auto add_field = [&](std::string_view name, std::string_view value) {
        publication += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
                       std::string(name) + "\"\r\n\r\n" + std::string(value) + "\r\n";
    };
    add_field("name", "lake-issue-3-fixture");
    add_field("version", "1.0.0");
    publication +=
        "--" + boundary + "\r\nContent-Disposition: form-data; name=\"content\"; filename=\"" +
        wheel.filename().string() + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
        wheel_bytes + "\r\n--" + boundary + "--\r\n";
    const std::string published = http_request(server.port(),
                                               "POST",
                                               "/legacy/",
                                               publication,
                                               "multipart/form-data; boundary=" + boundary,
                                               {},
                                               "Bearer registry-admin");
    ASSERT_NE(published.find(" 201 "), std::string::npos) << published;

    const std::string simple =
        http_request(server.port(), "GET", "/simple/lake-issue-3-fixture/", "", "text/html");
    ASSERT_NE(simple.find(wheel.filename().string()), std::string::npos) << simple;
    if (std::system("command -v pip3 >/dev/null 2>&1") == 0) {
        const std::filesystem::path target = temporary.path() / "python-installed";
        const std::filesystem::path install_log = temporary.path() / "pip-install.log";
        const std::string registry =
            "http://127.0.0.1:" + std::to_string(server.port()) + "/simple";
        const std::string install_command =
            "pip3 install --index-url '" + registry +
            "' --trusted-host 127.0.0.1"
            " --target '" +
            target.string() +
            "' --no-deps --disable-pip-version-check lake-issue-3-fixture==1.0.0 >'" +
            install_log.string() + "' 2>&1";
        ASSERT_EQ(std::system(install_command.c_str()), 0) << read_binary_file(install_log);
        EXPECT_EQ(read_binary_file(target / "lake_issue_3_fixture/__init__.py"),
                  "value = 'published through lake'\n");
    } else {
        const std::string downloaded = http_request(server.port(),
                                                    "GET",
                                                    "/packages/" + wheel.filename().string(),
                                                    "",
                                                    "application/octet-stream");
        EXPECT_EQ(response_body(downloaded), wheel_bytes);
    }
    server.stop();
}

TEST(RegistryProtocols, DebuginfodServesExecutableDebuginfoAndSource) {
    TemporarySyncRoot temporary;
    ServerOptions options;
    options.port = 0;
    options.node_name = "debug-registry-test";
    options.admin_token = "registry-admin";
    options.instance_kek = "registry-test-instance-kek";
    options.store_root = (temporary.path() / "store").string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string executable = read_binary_file(LAKE_TEST_DEBUG_FIXTURE_PATH);
    ASSERT_FALSE(executable.empty());
    const std::string build_id =
        lake::control::identity_crypto::sha256_hex(executable).substr(0, 40);
    const std::string debug_info = executable;
    const std::string source =
        read_binary_file(std::filesystem::path(LAKE_TEST_SOURCE_DIR) / "tests/debug_fixture.cpp");
    const auto publish =
        [&](std::string_view kind, std::string_view query, std::string_view contents) {
            return http_request(server.port(),
                                "PUT",
                                "/_lake/debug/" + build_id + "/" + std::string(kind) +
                                    std::string(query),
                                contents,
                                "application/octet-stream",
                                {},
                                "Bearer registry-admin");
        };
    EXPECT_NE(publish("executable", "", executable).find(" 201 "), std::string::npos);
    EXPECT_NE(publish("debuginfo", "", debug_info).find(" 201 "), std::string::npos);
    EXPECT_NE(publish("source", "?source=debug_fixture.cpp", source).find(" 201 "),
              std::string::npos);
    const auto fetch = [&](std::string_view suffix) {
        return response_body(http_request(server.port(),
                                          "GET",
                                          "/buildid/" + build_id + "/" + std::string(suffix),
                                          "",
                                          "application/octet-stream"));
    };
    EXPECT_EQ(fetch("executable"), executable);
    EXPECT_EQ(fetch("debuginfo"), debug_info);
    EXPECT_EQ(fetch("source/debug_fixture.cpp"), source);
    server.stop();
}

TEST(Registry, ServerInfoNeedsNoAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("server_info");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value["name"], "lake");
    EXPECT_EQ(r.value["store"], "memory");
}

TEST(Telemetry, PrometheusCounterAndHistogram) {
    lake::telemetry::Metrics metrics;
    metrics.increment_counter("test_requests_total", {{"result", "ok"}});
    metrics.increment_counter("test_requests_total", {{"result", "ok"}}, 2.0);
    metrics.observe_histogram("test_duration_seconds", {{"operation", "read"}}, 0.012);

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("# TYPE test_requests_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("test_requests_total{result=\"ok\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE test_duration_seconds histogram\n"), std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.01\"} 0\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"0.025\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_bucket{operation=\"read\",le=\"+Inf\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("test_duration_seconds_count{operation=\"read\"} 1\n"), std::string::npos);
}

TEST(Telemetry, StorePingIsMeasured) {
    lake::telemetry::Metrics metrics;
    auto store = lake::telemetry::instrument_store(std::make_shared<lake::control::InMemoryStore>(),
                                                   metrics);
    ASSERT_TRUE(store->ping());

    const std::string text = metrics.prometheus_text();
    EXPECT_NE(text.find("lake_store_calls_total{store=\"memory\",operation="
                        "\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
    EXPECT_NE(text.find("lake_store_call_duration_seconds_count{store=\"memory\","
                        "operation=\"ping\",result=\"ok\"} 1\n"),
              std::string::npos);
}

TEST(TelemetryHttp, HealthAndMemoryReadinessNeedNoAuth) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string health = http_get(server.port(), "/healthz");
    EXPECT_NE(health.find(" 200 "), std::string::npos);
    EXPECT_TRUE(health.ends_with("{\"ok\":true}"));

    const std::string ready = http_get(server.port(), "/readyz");
    EXPECT_NE(ready.find(" 200 "), std::string::npos);
    EXPECT_TRUE(ready.ends_with("{\"ok\":true,\"store\":\"memory\"}"));
    server.stop();
}

TEST(TelemetryHttp, MetricsRequireExistingAuthAndUsePrometheusContentType) {
    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/metrics");
    EXPECT_NE(denied.find(" 403 "), std::string::npos);
    EXPECT_NE(denied.find("\"code\":\"unauthorized\""), std::string::npos);

    const std::string served = http_get(server.port(), "/metrics", "Bearer test-token");
    EXPECT_NE(served.find(" 200 "), std::string::npos);
    EXPECT_NE(served.find("Content-Type: text/plain; version=0.0.4"), std::string::npos);
    EXPECT_NE(served.find("lake_build_info{version=\"0.0.1\",node=\"test-node\"} 1\n"),
              std::string::npos);
    EXPECT_NE(served.find("lake_up 1\n"), std::string::npos);
    EXPECT_NE(served.find("lake_http_requests_total{method=\"GET\",route=\"/"
                          "metrics\",status=\"403\"} 1\n"),
              std::string::npos);
    server.stop();
}

TEST(IdentityHttp, PagesIssueAndResolveHttpOnlySessions) {
    ServerOptions options;
    options.port = 0;
    options.public_host = "lake.example";
    options.admin_token = "operator-page-token";
    options.store = std::make_shared<lake::control::InMemoryStore>();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string page = http_get(server.port(), "/sign-up");
    EXPECT_NE(page.find(" 200 "), std::string::npos);
    EXPECT_NE(page.find("action=\"/identity/sign-up\""), std::string::npos);
    const std::string mfa_challenge = http_get(server.port(), "/mfa");
    EXPECT_NE(mfa_challenge.find(" 200 "), std::string::npos);
    EXPECT_NE(mfa_challenge.find("action=\"/identity/mfa/verify\""), std::string::npos);

    const std::string operator_page =
        http_get(server.port(), "/sign-in", "Bearer operator-page-token");
    EXPECT_NE(operator_page.find(" 302 "), std::string::npos);
    EXPECT_EQ(response_header(operator_page, "Location"), "/repositories");
    const std::string operator_sessions =
        http_get(server.port(), "/sessions", "Bearer operator-page-token");
    EXPECT_NE(operator_sessions.find(" 302 "), std::string::npos);
    EXPECT_EQ(response_header(operator_sessions, "Location"), "/repositories");

    const std::string created =
        http_request(server.port(),
                     "POST",
                     "/identity/sign-up",
                     "email=page%40example.test&display_name=Page+User&password=correct+horse+"
                     "battery+staple&device_name=browser&identity_document=%7B%7D",
                     "application/x-www-form-urlencoded");
    EXPECT_NE(created.find(" 302 "), std::string::npos) << created;
    EXPECT_EQ(response_header(created, "Location"), "/sessions");
    const std::string set_cookie = response_header(created, "Set-Cookie");
    EXPECT_NE(set_cookie.find("HttpOnly"), std::string::npos);
    const std::size_t cookie_end = set_cookie.find(';');
    const std::string cookie = set_cookie.substr(0, cookie_end);

    const std::string sessions =
        http_request(server.port(), "GET", "/sessions", "", "text/plain", cookie);
    EXPECT_NE(sessions.find(" 200 "), std::string::npos) << sessions;
    EXPECT_NE(sessions.find("Sessions and devices"), std::string::npos);
    EXPECT_NE(sessions.find("browser"), std::string::npos);

    const std::string mfa_enrollment = http_request(server.port(),
                                                    "POST",
                                                    "/identity/mfa/enroll",
                                                    "",
                                                    "application/x-www-form-urlencoded",
                                                    cookie);
    EXPECT_NE(mfa_enrollment.find(" 200 "), std::string::npos) << mfa_enrollment;
    EXPECT_NE(mfa_enrollment.find("otpauth://totp/lake:"), std::string::npos);
    EXPECT_NE(mfa_enrollment.find("Recovery codes"), std::string::npos);
    EXPECT_NE(mfa_enrollment.find("action=\"/identity/mfa/disable\""), std::string::npos);

    const std::string rest_signup = http_request(
        server.port(),
        "POST",
        "/v1/sign_up_password",
        R"({"email":"rest-page@example.test","password":"correct horse battery staple","display_name":"REST Page","device_name":"browser","identity_document":"{}"})",
        "application/json");
    EXPECT_NE(rest_signup.find(" 200 "), std::string::npos) << rest_signup;
    EXPECT_NE(response_header(rest_signup, "Set-Cookie").find("HttpOnly"), std::string::npos);
    server.stop();
}

TEST(ApplicationHttp, HostedAndCustomDomainPagesUseTheApplicationPool) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = store->create_user(
        {"taap:1:ed25519:http-app-owner", "http-app-owner@example.test", "HTTP owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        store->create_customer({"individual", owner->id, "", "http-application-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const std::string application_id = lake::control::new_uuid_v4();
    const auto key = lake::control::apps::generate_signing_key(application_id, 1);
    ASSERT_TRUE(key) << key.error();
    const auto application =
        store->create_application({.id = application_id,
                                   .customer_id = customer->id,
                                   .pool_id = "app:" + application_id,
                                   .name = "Hosted product",
                                   .allowed_origins = {"https://product.example.test"},
                                   .redirect_uris = {"https://product.example.test/callback"},
                                   .custom_sign_in_domain = "login.product.example.test",
                                   .theme = {{"accent_color", "#123456"}},
                                   .signing_key = *key});
    ASSERT_TRUE(application) << application.error().message;

    ServerOptions options;
    options.port = 0;
    options.public_host = "lake.example.test";
    options.store = store;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string hosted = http_request(
        server.port(), "GET", "/apps/" + application_id + "/sign-up", "", "text/plain");
    EXPECT_NE(hosted.find(" 200 "), std::string::npos) << hosted;
    EXPECT_NE(hosted.find("Hosted product"), std::string::npos);
    EXPECT_NE(hosted.find("--accent:#123456"), std::string::npos);
    EXPECT_NE(hosted.find("id=\"theme-preference\""), std::string::npos);
    EXPECT_NE(hosted.find("id=\"theme-toggle-script\""), std::string::npos);

    const std::string custom = http_request(
        server.port(), "GET", "/sign-in", "", "text/plain", "", "", "login.product.example.test");
    EXPECT_NE(custom.find(" 200 "), std::string::npos) << custom;
    EXPECT_NE(custom.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom.find("/apps/" + application_id + "/identity/sign-in"), std::string::npos);

    const std::string custom_mfa = http_request(
        server.port(), "GET", "/mfa", "", "text/plain", "", "", "login.product.example.test");
    EXPECT_NE(custom_mfa.find(" 200 "), std::string::npos) << custom_mfa;
    EXPECT_NE(custom_mfa.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom_mfa.find("/apps/" + application_id + "/identity/mfa/verify"),
              std::string::npos);

    const std::string custom_passkey = http_request(server.port(),
                                                    "GET",
                                                    "/passkey-sign-in",
                                                    "",
                                                    "text/plain",
                                                    "",
                                                    "",
                                                    "login.product.example.test");
    EXPECT_NE(custom_passkey.find(" 200 "), std::string::npos) << custom_passkey;
    EXPECT_NE(custom_passkey.find("Hosted product"), std::string::npos);
    EXPECT_NE(custom_passkey.find("navigator.credentials.get"), std::string::npos);

    const std::string created = http_request(
        server.port(),
        "POST",
        "/apps/" + application_id + "/identity/sign-up",
        "email=hosted%40example.test&display_name=Hosted+User&password=correct+horse+battery+"
        "staple&device_name=browser&identity_document=%7B%7D",
        "application/x-www-form-urlencoded");
    EXPECT_NE(created.find(" 302 "), std::string::npos) << created;
    EXPECT_TRUE(response_header(created, "Location")
                    .starts_with("https://product.example.test/callback#access_token="));
    EXPECT_NE(response_header(created, "Location").find("&token_type=Bearer&expires_in=900"),
              std::string::npos);
    EXPECT_NE(response_header(created, "Set-Cookie").find("HttpOnly"), std::string::npos);

    const std::string signed_in = http_request(
        server.port(),
        "POST",
        "/apps/" + application_id + "/identity/sign-in",
        "email=hosted%40example.test&password=correct+horse+battery+staple&device_name=browser",
        "application/x-www-form-urlencoded");
    EXPECT_NE(signed_in.find(" 302 "), std::string::npos) << signed_in;

    const std::string jwks = http_request(server.port(),
                                          "GET",
                                          "/v1/apps/" + application_id + "/.well-known/jwks.json",
                                          "",
                                          "application/json");
    EXPECT_NE(jwks.find(" 200 "), std::string::npos) << jwks;
    const json keys = response_json(jwks);
    ASSERT_EQ(keys.at("keys").size(), 1);
    EXPECT_EQ(keys.at("keys").at(0).at("kid"), key->kid);
    server.stop();
}

TEST(ScimHttp, RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess) {
    auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto organization = store->create_organization({"", "SCIM HTTP", ""});
    ASSERT_TRUE(organization.has_value());
    const std::string token = "recorded-scim-token";
    const auto provider = store->register_sso_provider({organization->id,
                                                        "lake",
                                                        "recorded-client",
                                                        "oidc",
                                                        "https://identity.example.test",
                                                        "",
                                                        "",
                                                        "recorded-client-id",
                                                        "recorded-client-secret",
                                                        token,
                                                        json::object(),
                                                        false});
    ASSERT_TRUE(provider.has_value());

    ServerOptions options;
    options.port = 0;
    options.store = store;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string denied = http_get(server.port(), "/scim/v2/Users");
    EXPECT_NE(denied.find(" 401 "), std::string::npos) << denied;
    EXPECT_NE(denied.find("urn:ietf:params:scim:api:messages:2.0:Error"), std::string::npos);

    const auto create_user = [&](std::string_view external_id,
                                 std::string_view email,
                                 std::string_view display_name,
                                 std::string_view taap_id) {
        const json body{
            {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:User"})},
            {"externalId", external_id},
            {"userName", email},
            {"displayName", display_name},
            {"active", true},
            {"urn:orama:params:scim:schemas:extension:taap:2.0:User", {{"taapId", taap_id}}}};
        return http_request(server.port(),
                            "POST",
                            "/scim/v2/Users",
                            body.dump(),
                            "application/scim+json",
                            "",
                            "Bearer " + token);
    };
    const std::string first_response =
        create_user("user-one", "one@example.test", "One", "taap:1:ed25519:user-one");
    EXPECT_NE(first_response.find(" 201 "), std::string::npos) << first_response;
    const json first = response_json(first_response);
    ASSERT_TRUE(first.is_object());
    const std::string first_id = first.at("id");
    const std::string second_response =
        create_user("user-two", "two@example.test", "Two", "taap:1:ed25519:user-two");
    EXPECT_NE(second_response.find(" 201 "), std::string::npos) << second_response;
    const json second = response_json(second_response);
    ASSERT_TRUE(second.is_object());
    const std::string second_id = second.at("id");

    const std::string filtered =
        http_get(server.port(),
                 "/scim/v2/Users?filter=userName%20eq%20%22one%40example.test%22",
                 "Bearer " + token);
    EXPECT_NE(filtered.find(" 200 "), std::string::npos) << filtered;
    const json filtered_body = response_json(filtered);
    ASSERT_TRUE(filtered_body.is_object());
    EXPECT_EQ(filtered_body.at("totalResults"), 1);
    EXPECT_EQ(filtered_body.at("Resources").at(0).at("id"), first_id);

    const json user_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "replace"}, {"path", "displayName"}, {"value", "One Updated"}}})}};
    const std::string patched_user = http_request(server.port(),
                                                  "PATCH",
                                                  "/scim/v2/Users/" + first_id,
                                                  user_patch.dump(),
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(patched_user.find(" 200 "), std::string::npos) << patched_user;
    EXPECT_EQ(response_json(patched_user).at("displayName"), "One Updated");

    const json group_body{
        {"schemas", json::array({"urn:ietf:params:scim:schemas:core:2.0:Group"})},
        {"externalId", "engineering"},
        {"displayName", "Engineering"},
        {"members", json::array({{{"value", first_id}}, {{"value", second_id}}})}};
    const std::string group_response = http_request(server.port(),
                                                    "POST",
                                                    "/scim/v2/Groups",
                                                    group_body.dump(),
                                                    "application/scim+json",
                                                    "",
                                                    "Bearer " + token);
    EXPECT_NE(group_response.find(" 201 "), std::string::npos) << group_response;
    const json group = response_json(group_response);
    ASSERT_TRUE(group.is_object());
    const std::string group_id = group.at("id");
    auto memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 2);

    const json group_patch{
        {"schemas", json::array({"urn:ietf:params:scim:api:messages:2.0:PatchOp"})},
        {"Operations",
         json::array({{{"op", "remove"}, {"path", "members[value eq \"" + first_id + "\"]"}}})}};
    const std::string patched_group = http_request(server.port(),
                                                   "PATCH",
                                                   "/scim/v2/Groups/" + group_id,
                                                   group_patch.dump(),
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(patched_group.find(" 200 "), std::string::npos) << patched_group;
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_EQ(memberships->size(), 1);
    EXPECT_EQ(memberships->at(0).principal_id, second.at("id").get<std::string>());

    ASSERT_TRUE(
        store->create_federation_issuer({organization->id, provider->issuer, "email"}).has_value());
    const auto session = store->sign_in_sso({provider->id,
                                             provider->issuer,
                                             "user-two",
                                             "two@example.test",
                                             "Two",
                                             "",
                                             "recorded-client",
                                             {{"email", "two@example.test"}}});
    ASSERT_TRUE(session.has_value());
    const std::string deleted_user = http_request(server.port(),
                                                  "DELETE",
                                                  "/scim/v2/Users/" + second_id,
                                                  "",
                                                  "application/scim+json",
                                                  "",
                                                  "Bearer " + token);
    EXPECT_NE(deleted_user.find(" 204 "), std::string::npos) << deleted_user;
    EXPECT_FALSE(store->context_for(session->secret).has_value());
    memberships = store->list_memberships(organization->id, group_id, 100);
    ASSERT_TRUE(memberships.has_value());
    EXPECT_TRUE(memberships->empty());

    const std::string deleted_group = http_request(server.port(),
                                                   "DELETE",
                                                   "/scim/v2/Groups/" + group_id,
                                                   "",
                                                   "application/scim+json",
                                                   "",
                                                   "Bearer " + token);
    EXPECT_NE(deleted_group.find(" 204 "), std::string::npos) << deleted_group;
    EXPECT_FALSE(store->get_team(group_id).has_value());
    const auto events = store->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events.has_value());
    EXPECT_TRUE(std::ranges::any_of(*events, [](const lake::audit::Event& event) {
        return event.draft.action == "scim_delete_user" && event.draft.outcome == "success";
    }));
    server.stop();
}

TEST(Registry, EverythingElseNeedsAuth) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult r = s.call("list_users");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.code, "unauthorized");
    EXPECT_EQ(rest_status(r), 403);
}

TEST(Registry, ValidatesInput) {
    Surface s;
    ApiResult r = s.call("create_customer", json::object());
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(r.error.path, "/customer_type");
    r = s.call("get_user", {{"id", "x"}, {"bogus", 1}});
    EXPECT_EQ(r.error.path, "/bogus");
    r = s.call("list_users", {{"limit", "ten"}});
    EXPECT_EQ(r.error.code, "invalid_input");
    EXPECT_EQ(s.call("no_such").error.code, "unknown_capability");
}

TEST(Audit, RegistryRecordsEachCapabilityOutcomeAndDetectsDeletedRows) {
    Surface s;
    for (const auto& capability : s.registry.list()) {
        s.call(capability->name);
    }
    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), s.registry.list().size());
    for (const auto& capability : s.registry.list()) {
        const auto count =
            std::count_if(events->begin(), events->end(), [&](const lake::audit::Event& event) {
                return event.draft.action == capability->name;
            });
        EXPECT_EQ(count, 1) << capability->name;
    }
    EXPECT_TRUE(lake::audit::verify(s.store, *events));

    std::vector<lake::audit::Event> deleted = *events;
    deleted.erase(deleted.begin() + 1);
    const auto verification = lake::audit::verify(s.store, deleted);
    EXPECT_FALSE(verification);
    EXPECT_EQ(verification.error().code, "invalid_chain");
}

TEST(Audit, DeniedAuthorizationAndJsonLinesExportAreRecorded) {
    Surface s;
    s.ctx.authorized = false;
    const ApiResult denied = s.call("list_users");
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");

    s.ctx.authorized = true;
    const ApiResult exported = s.call("export_audit");
    ASSERT_TRUE(exported.ok) << exported.error.message;
    std::istringstream lines(exported.value["jsonl"].get<std::string>());
    std::string line;
    std::vector<json> decoded;
    while (std::getline(lines, line)) {
        if (!line.empty()) {
            decoded.push_back(json::parse(line));
        }
    }
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]["outcome"], "denied");
    EXPECT_EQ(decoded[0]["action"], "list_users");

    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    ASSERT_EQ(events->size(), 2u);
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(Audit, IdentityRotatesAndRetentionFloorIsEnforced) {
    Surface s;
    const ApiResult initial = s.call("identity");
    ASSERT_TRUE(initial.ok) << initial.error.message;
    const std::string initial_identity = initial.value["taap_id"];
    const ApiResult rotated = s.call("rotate_identity");
    ASSERT_TRUE(rotated.ok) << rotated.error.message;
    EXPECT_NE(rotated.value["taap_id"], initial_identity);

    lake::control::InMemoryStore externally_seeded_store;
    const lake::audit::IdentityOptions externally_seeded_options{
        .configured_seed = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
        .instance_kek = "test-instance-kek",
    };
    auto externally_seeded =
        lake::audit::identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_seeded) << externally_seeded.error().message;
    auto externally_rotated =
        lake::audit::rotate_identity(externally_seeded_store, externally_seeded_options);
    ASSERT_TRUE(externally_rotated) << externally_rotated.error().message;
    lake::audit::EventDraft externally_seeded_event{
        .kind = "administrative",
        .principal = "admin",
        .object = "lake",
        .action = "verify_external_rotation",
        .outcome = "success",
        .request_id = lake::control::new_uuid_v4(),
    };
    auto recorded = lake::audit::record(
        externally_seeded_store, externally_seeded_event, externally_seeded_options);
    ASSERT_TRUE(recorded) << recorded.error().message;
    auto externally_seeded_events =
        externally_seeded_store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(externally_seeded_events);
    EXPECT_TRUE(lake::audit::verify(externally_seeded_store, *externally_seeded_events));

    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult accepted =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 500}});
    EXPECT_TRUE(accepted.ok);
    const ApiResult lowered =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 400}});
    EXPECT_FALSE(lowered.ok);
    EXPECT_EQ(lowered.error.code, "conflict");
    const ApiResult refused =
        s.call("set_audit_retention", {{"customer_id", customer.value["id"]}, {"days", 399}});
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");

    auto events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(events);
    const auto retention =
        std::find_if(events->rbegin(), events->rend(), [](const lake::audit::Event& event) {
            return event.draft.kind == "retention" && event.draft.outcome == "failure";
        });
    ASSERT_NE(retention, events->rend());
    EXPECT_EQ(retention->draft.action, "set_audit_retention");
    EXPECT_TRUE(lake::audit::verify(s.store, *events));
}

TEST(PostgresAudit, RegistryEventsPersistAndVerify) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    auto store = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = store->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const auto before = store->list_audit_events(lake::audit::Filter{.limit = 1000000});
    ASSERT_TRUE(before) << before.error().message;
    Registry registry;
    register_core_capabilities(registry, "0.0.1");
    ApiContext context;
    context.store = store.get();
    context.node_name = "postgres-test";
    context.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    context.authorized = true;
    context.acting_identity = "admin";

    for (const auto& capability : registry.list()) {
        context.request_id.clear();
        registry.call(capability->name, json::object(), context);
    }
    context.authorized = false;
    context.request_id.clear();
    const ApiResult result = registry.call("list_users", json::object(), context);
    EXPECT_EQ(result.error.code, "unauthorized");

    auto events = store->list_audit_events(lake::audit::Filter{.limit = 1000000});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_EQ(events->size(), before->size() + registry.list().size() + 1);
    EXPECT_EQ(events->back().draft.outcome, "denied");
    const auto verification = lake::audit::verify(*store, *events);
    EXPECT_TRUE(verification) << verification.error().message;
}

TEST(Customers, IndividualLifecycleWithGeneratedHandle) {
    Surface s;
    const ApiResult user =
        s.call("create_user", {{"email", "Ada@Example.com"}, {"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const std::string user_id = user.value["id"];
    EXPECT_EQ(user_id.size(), 36u);

    const ApiResult customer =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const std::string name = customer.value["name"];
    EXPECT_TRUE(lake::control::valid_handle(name)) << name;
    EXPECT_EQ(customer.value["user_id"], user_id);
    EXPECT_TRUE(customer.value["organization_id"].is_null());

    const ApiResult again =
        s.call("create_customer", {{"customer_type", "individual"}, {"user_id", user_id}});
    EXPECT_EQ(again.error.code, "conflict"); // one customer per user
    EXPECT_EQ(rest_status(again), 409);

    const ApiResult dup_email = s.call("create_user", {{"email", "ada@example.com"}});
    EXPECT_EQ(dup_email.error.code, "conflict"); // case-insensitive email

    const ApiResult listed = s.call("list_customers");
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value["items"].size(), 1u);
    const ApiResult got = s.call("get_customer", {{"id", customer.value["id"]}});
    ASSERT_TRUE(got.ok);
    EXPECT_EQ(got.value["name"], name);
    EXPECT_EQ(s.call("get_customer", {{"id", "missing"}}).error.code, "not_found");
}
TEST(Customers, OrganizationAndTheTenantRules) {
    Surface s;
    const ApiResult org = s.call("create_organization",
                                 {{"display_name", "Orama"}, {"taap_id", "taap:1:ed25519:abc"}});
    ASSERT_TRUE(org.ok);
    const std::string org_id = org.value["id"];
    EXPECT_EQ(s.call("create_organization", {{"taap_id", "taap:1:ed25519:abc"}}).error.code,
              "conflict");

    // xor: an organization customer must not carry a user_id; type must match the
    // id given.
    ApiResult bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"user_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad = s.call("create_customer", {{"customer_type", "individual"}, {"organization_id", org_id}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    bad =
        s.call("create_customer", {{"customer_type", "organization"}, {"organization_id", "nope"}});
    EXPECT_EQ(bad.error.code, "invalid_input"); // FK

    // Handle format and the single global namespace.
    bad = s.call(
        "create_customer",
        {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "Orama Inc"}});
    EXPECT_EQ(bad.error.code, "invalid_input");
    const ApiResult ok =
        s.call("create_customer",
               {{"customer_type", "organization"}, {"organization_id", org_id}, {"name", "orama"}});
    ASSERT_TRUE(ok.ok) << ok.error.message;
    const ApiResult user = s.call("create_user", {{"display_name", "Bob"}});
    const ApiResult taken =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "orama"}});
    EXPECT_EQ(taken.error.code, "conflict");
}

TEST(Rest, PathsMethodsAndQueryCoercion) {
    Surface s;
    const auto list = s.registry.find("list_users");
    const auto create = s.registry.find("create_user");
    EXPECT_EQ(rest_path(*list), "/v1/list_users");
    EXPECT_EQ(rest_method(*list), "GET");
    EXPECT_EQ(rest_method(*create), "POST");
    const json input = rest_input_from_query(*list, {{"limit", "5"}, {"other", "x"}});
    EXPECT_EQ(input["limit"], 5);
    EXPECT_EQ(input["other"], "x");
    const json envelope = rest_envelope(ApiResult::failure("not_found", "gone", "/id"));
    EXPECT_FALSE(envelope["ok"]);
    EXPECT_EQ(envelope["error"]["code"], "not_found");
    EXPECT_EQ(rest_status(ApiResult::failure("mfa_required", "verify MFA")), 403);
    EXPECT_EQ(rest_status(ApiResult::failure("store_unavailable", "")), 503);
}

TEST(Rest, OpenApiCoversEveryCapability) {
    Surface s;
    const json doc = openapi_document(s.registry, "0.0.1");
    EXPECT_EQ(doc["openapi"], "3.1.0");
    for (const auto& capability : s.registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        const std::string method = rest_method(*capability) == "GET" ? "get" : "post";
        ASSERT_TRUE(doc["paths"].contains(rest_path(*capability))) << capability->name;
        EXPECT_TRUE(doc["paths"][rest_path(*capability)].contains(method)) << capability->name;
    }
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/push"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/pull"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/find-missing"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks"));
    EXPECT_TRUE(doc["paths"].contains("/v1/repositories/{repository_id}/sync/chunks/{digest}"));
    EXPECT_TRUE(doc["paths"].contains("/v1/attach_model_provenance"));
    EXPECT_TRUE(doc["paths"].contains("/v1/verify_model_provenance"));
    EXPECT_TRUE(doc["paths"].contains("/v1/resolve_conflict"));
    const json& conflict =
        doc["paths"]["/v1/list_conflicts"]["get"]["responses"]["200"]["content"]["application/json"]
           ["schema"]["properties"]["value"]["properties"]["items"]["items"];
    EXPECT_EQ(conflict["properties"]["kind"]["enum"],
              json::array({"text", "structural", "crdt-suspicious", "suspicious-merge"}));
    EXPECT_EQ(conflict["properties"]["region"]["properties"]["path"]["type"], "string");
    EXPECT_EQ(conflict["properties"]["explanation"]["type"], "string");
    EXPECT_TRUE(doc["paths"].contains("/v1/transparency_inclusion_proof"));
    EXPECT_TRUE(doc["paths"].contains("/v1/transparency_consistency_proof"));
    const json& inclusion_value =
        doc["paths"]["/v1/transparency_inclusion_proof"]["get"]["responses"]["200"]["content"]
           ["application/json"]["schema"]["properties"]["value"];
    EXPECT_EQ(inclusion_value["properties"]["audit_path"]["items"]["pattern"], "^[0-9a-f]{64}$");
    EXPECT_EQ(inclusion_value["properties"]["checkpoint"]["type"], "object");
    EXPECT_TRUE(doc["paths"].contains("/mcp"));
    EXPECT_TRUE(doc["paths"]["/v1/server_info"]["get"]["security"].empty());
    const json& push = doc["paths"]["/v1/repositories/{repository_id}/sync/push"]["post"];
    const json& request_header = push["requestBody"]["content"]["application/vnd.liquid.sync-v1"]
                                     ["schema"]["x-liquid-sync-header-schema"];
    EXPECT_EQ(request_header["properties"]["atomic"]["type"], "boolean");
    const json& outcome =
        push["responses"]["200"]["content"]["application/vnd.liquid.sync-v1"]["schema"]
            ["x-liquid-sync-header-schema"]["properties"]["outcomes"]["items"];
    EXPECT_EQ(outcome["properties"]["name"]["type"], "string");
    EXPECT_EQ(outcome["properties"]["outcome"]["enum"], json::array({"applied", "refused"}));
    EXPECT_EQ(outcome["properties"]["reason"]["type"], "string");
}

TEST(SyncRegistry, DeclaresTransportPolicyAndRecordsDenial) {
    Surface surface;
    const auto push = surface.registry.find("push");
    const auto pull = surface.registry.find("pull");
    const auto refs = surface.registry.find("refs");
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    ASSERT_NE(refs, nullptr);
    EXPECT_EQ(push->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(push->action, "write");
    EXPECT_EQ(pull->transport, CapabilityTransport::LiquidSync);
    EXPECT_EQ(pull->action, "read");
    EXPECT_EQ(refs->transport, CapabilityTransport::Json);

    surface.ctx.authorized = false;
    const ApiResult denied = surface.call("push", {{"repository_id", "missing"}});
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    const auto events = surface.store.list_audit_events({.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_FALSE(events->empty());
    EXPECT_EQ(events->back().draft.action, "push");
    EXPECT_EQ(events->back().draft.outcome, "denied");
}

TEST(SyncRegistry, RevokedOfflineGrantRefusesPublicationWithChangeAndReason) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    const auto issued = surface.store.sign_up_password({"lake",
                                                        "offline-writer@example.test",
                                                        "correct horse battery staple",
                                                        "Offline writer",
                                                        "offline device",
                                                        "{}"});
    ASSERT_TRUE(issued) << issued.error().message;
    const auto principal = surface.store.context_for(issued->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto customer =
        surface.store.create_customer({"individual", principal->user_id, "", "offline-writer"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        surface.store.create_repository({customer->id, "offline-sync", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto object = surface.store.find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(surface.store.create_grant({principal->taap_id,
                                            object->id,
                                            "write",
                                            "offline-sync-attestation",
                                            "admin",
                                            "offline-sync"}));
    surface.ctx.authorized = true;
    surface.ctx.operator_credential = false;
    surface.ctx.session_credential = true;
    surface.ctx.acting_user_id = principal->user_id;
    surface.ctx.acting_identity = principal->taap_id;
    surface.ctx.credential_device_id = principal->device_id;
    surface.ctx.scopes = principal->scopes;
    const std::string scope = "repository:" + repository->id + ":read";
    const ApiResult grant = surface.call("issue_offline_grant",
                                         {{"device_id", principal->device_id},
                                          {"scopes", json::array({scope})},
                                          {"duration_seconds", 3600}});
    ASSERT_TRUE(grant.ok) << grant.error.message;
    ASSERT_TRUE(surface.call("revoke_offline_grant", {{"id", grant.value["id"]}}).ok);

    surface.ctx.sync_store = &repository_store;
    surface.ctx.sync_wire_request = true;
    surface.ctx.offline_grant_id = grant.value["id"];
    surface.ctx.reauthorization_results = json::array();
    const ApiResult pushed = surface.call("push", {{"repository_id", repository->id}});
    ASSERT_FALSE(pushed.ok);
    EXPECT_EQ(pushed.error.code, "unauthorized");
    ASSERT_EQ(surface.ctx.reauthorization_results.size(), 1U);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["change"], "change-refused");
    EXPECT_FALSE(surface.ctx.reauthorization_results[0]["allowed"]);
    EXPECT_EQ(surface.ctx.reauthorization_results[0]["reason"],
              "offline grant was revoked before publication");
    const auto events = surface.store.list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "denied" &&
               event.draft.details.value("change", "") == "change-refused" &&
               event.draft.details.value("reason", "") ==
                   "offline grant was revoked before publication";
    }));
}

TEST(Mcp, InitializeListAndCall) {
    Surface s;
    const auto init = mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, s.ctx, "0.0.1");
    ASSERT_TRUE(init);
    EXPECT_EQ(json::parse(*init)["result"]["serverInfo"]["name"], "lake");
    EXPECT_FALSE(mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, s.ctx, "0.0.1"));

    const auto tools = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, s.ctx, "0.0.1"));
    const auto json_capabilities = std::ranges::count_if(s.registry.list(), [](const auto& entry) {
        return entry->transport == CapabilityTransport::Json;
    });
    EXPECT_EQ(tools["result"]["tools"].size(), json_capabilities);
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "attach_model_provenance";
    }));
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "verify_model_provenance";
    }));
    const auto inclusion_tool =
        std::ranges::find_if(tools["result"]["tools"], [](const json& tool) {
            return tool.value("name", "") == "transparency_inclusion_proof";
        });
    ASSERT_NE(inclusion_tool, tools["result"]["tools"].end());
    EXPECT_TRUE(inclusion_tool->at("outputSchema").at("properties").contains("audit_path"));
    EXPECT_TRUE(std::ranges::any_of(tools["result"]["tools"], [](const json& tool) {
        return tool.value("name", "") == "transparency_consistency_proof";
    }));
    const auto resolve_conflict =
        std::ranges::find_if(tools["result"]["tools"], [](const json& tool) {
            return tool.value("name", "") == "resolve_conflict";
        });
    ASSERT_NE(resolve_conflict, tools["result"]["tools"].end());
    EXPECT_EQ(resolve_conflict->at("inputSchema").at("properties").at("resolution").at("enum"),
              json::array({"accept_proposal", "left", "right", "blob"}));

    const json call = {
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "tools/call"},
        {"params", {{"name", "create_user"}, {"arguments", {{"display_name", "Eve"}}}}}};
    const auto reply = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_FALSE(reply["result"]["isError"]);
    EXPECT_EQ(reply["result"]["structuredContent"]["value"]["display_name"], "Eve");

    s.ctx.authorized = false;
    const auto denied = json::parse(*mcp_dispatch(s.registry, call, s.ctx, "0.0.1"));
    EXPECT_TRUE(denied["result"]["isError"]);
    EXPECT_EQ(denied["result"]["structuredContent"]["error"]["code"], "unauthorized");
    const auto unknown = json::parse(*mcp_dispatch(
        s.registry, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "nope"}}, s.ctx, "0.0.1"));
    EXPECT_EQ(unknown["error"]["code"], -32601);
}

TEST(Repositories, OwnedByACustomerNamedWithinIt) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const std::string cid = customer.value["id"];

    ApiResult repo =
        s.call("create_repository",
               {{"customer_id", cid}, {"name", "liquid"}, {"description", "the tool"}});
    ASSERT_TRUE(repo.ok) << repo.error.message;
    EXPECT_EQ(repo.value["visibility"], "private");
    EXPECT_EQ(repo.value["customer_id"], cid);

    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "liquid"}}).error.code,
              "conflict");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", cid}, {"name", "Bad Name"}}).error.code,
              "invalid_input");
    EXPECT_EQ(s.call("create_repository", {{"customer_id", "nope"}, {"name", "x"}}).error.code,
              "invalid_input");
    EXPECT_EQ(
        s.call("create_repository", {{"customer_id", cid}, {"name", "y"}, {"visibility", "secret"}})
            .error.code,
        "invalid_input");
    ASSERT_TRUE(s.call("create_repository",
                       {{"customer_id", cid}, {"name", "lake"}, {"visibility", "public"}})
                    .ok);

    const ApiResult all = s.call("list_repositories");
    ASSERT_TRUE(all.ok);
    EXPECT_EQ(all.value["items"].size(), 2u);
    EXPECT_EQ(all.value["items"][0]["name"], "lake"); // newest first
    const ApiResult mine = s.call("list_repositories", {{"customer_id", cid}, {"limit", 1}});
    EXPECT_EQ(mine.value["items"].size(), 1u);
    const ApiResult none = s.call("list_repositories", {{"customer_id", "other"}});
    EXPECT_EQ(none.value["items"].size(), 0u);
    EXPECT_EQ(s.call("get_repository", {{"id", repo.value["id"]}}).value["name"], "liquid");
    EXPECT_EQ(s.call("get_repository", {{"id", "missing"}}).error.code, "not_found");
}

TEST(Forge, PersistsIssueLifecycleAndMapsReviewCommentAnchors) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "forge"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];

    const ApiResult issue = surface.call(
        "create_issue",
        {{"repository_id", repository_id},
         {"title", "Document anchors"},
         {"body", "Discuss `src/lib.cpp`."},
         {"labels", json::array({"docs"})},
         {"assignees", json::array({"taap:1:ed25519:ada"})},
         {"references", json::array({{"change", "change-1"}, {"path", "src/lib.cpp"}})}});
    ASSERT_TRUE(issue.ok) << issue.error.message;
    EXPECT_EQ(issue.value["value"]["state"], "open");
    EXPECT_TRUE(surface
                    .call("update_issue",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"state", "closed"}})
                    .ok);
    EXPECT_TRUE(surface
                    .call("create_issue_comment",
                          {{"repository_id", repository_id},
                           {"issue_id", issue.value["id"]},
                           {"body", "Resolved by change-2."}})
                    .ok);

    const ApiResult review = surface.call("create_review",
                                          {{"repository_id", repository_id},
                                           {"change_id", "change-1"},
                                           {"reviewers", json::array({"taap:1:ed25519:ada"})}});
    ASSERT_TRUE(review.ok) << review.error.message;
    const ApiResult comment = surface.call(
        "create_review_comment",
        {{"repository_id", repository_id},
         {"review_id", review.value["id"]},
         {"body", "Use a more specific name."},
         {"anchor",
          {{"change", "change-1"}, {"path", "src/lib.cpp"}, {"line", 8}, {"column", 4}}}});
    ASSERT_TRUE(comment.ok) << comment.error.message;

    const ApiResult mapped = surface.call(
        "map_review_anchors",
        {{"repository_id", repository_id},
         {"source_change", "change-1"},
         {"target_change", "change-2"},
         {"path", "src/lib.cpp"},
         {"hunks",
          json::array({{{"old_line", 1}, {"old_count", 0}, {"new_line", 1}, {"new_count", 2}}})}});
    ASSERT_TRUE(mapped.ok) << mapped.error.message;
    ASSERT_EQ(mapped.value["items"].size(), 1);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["change"], "change-2");
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["line"], 10);
    EXPECT_EQ(mapped.value["items"][0]["value"]["anchor"]["disposition"], "shifted");
}

TEST(Export, VerifiesAndRecreatesPortableRepositoryFacts) {
    Surface source;
    RepositoryFixtureStore source_store;
    source.ctx.sync_store = &source_store;
    const ApiResult user = source.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = source.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = source.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "portable"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    ASSERT_TRUE(source
                    .call("create_issue",
                          {{"repository_id", repository.value["id"]}, {"title", "Portable"}})
                    .ok);
    const auto object = source.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(object) << object.error().message;
    ASSERT_TRUE(
        source.store
            .create_grant(
                {"taap:1:ed25519:reader", object->id, "read", "grant-proof", "admin", "test"})
            .has_value());

    const ApiResult exported = source.call(
        "export_repository", {{"repository_id", repository.value["id"]}, {"include_git", true}});
    ASSERT_TRUE(exported.ok) << exported.error.message;
    EXPECT_EQ(exported.value["archive"]["format"], "lake_export/1");

    Surface destination;
    RepositoryFixtureStore destination_store;
    destination.ctx.sync_store = &destination_store;
    const ApiResult target_user = destination.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(target_user.ok) << target_user.error.message;
    const ApiResult target_customer = destination.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", target_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(target_customer.ok) << target_customer.error.message;
    const ApiResult imported = destination.call("import_repository",
                                                {{"customer_id", target_customer.value["id"]},
                                                 {"name", "portable-copy"},
                                                 {"archive", exported.value["archive"]}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    const auto portable =
        destination_store.imported.find(target_customer.value["id"].get<std::string>() + "/" +
                                        imported.value["id"].get<std::string>());
    ASSERT_NE(portable, destination_store.imported.end());
    EXPECT_EQ(portable->second.history, exported.value["archive"]["payload"]["history"]);
    EXPECT_EQ(portable->second.artifacts, exported.value["archive"]["payload"]["artifacts"]);
    EXPECT_EQ(destination.call("list_issues", {{"repository_id", imported.value["id"]}})
                  .value["items"]
                  .size(),
              1);

    json altered = exported.value["archive"];
    altered["payload"]["history"]["changes"][0]["message"] = "changed";
    EXPECT_EQ(destination
                  .call("import_repository",
                        {{"customer_id", target_customer.value["id"]},
                         {"name", "tampered"},
                         {"archive", altered}})
                  .error.code,
              "invalid_digest");
}

TEST(GitHubImport, MapsRecordedIssuesReviewsAndWorkflowReport) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                        "github-import-recording.json");
    ASSERT_TRUE(input.good());
    const json recording = json::parse(input);
    const ApiResult imported = surface.call("import_github_repository",
                                            {{"customer_id", customer.value["id"]},
                                             {"name", "example"},
                                             {"git_directory", "/recorded/example.git"},
                                             {"recording", recording}});
    ASSERT_TRUE(imported.ok) << imported.error.message;
    EXPECT_EQ(imported.value["fidelity"]["format"], "github_import_fidelity/1");
    EXPECT_EQ(imported.value["fidelity"]["workflows"][0]["wrap_replacements"][0]["replacement"],
              "liquid wrap");
    EXPECT_EQ(surface.call("list_issues", {{"repository_id", imported.value["repository"]["id"]}})
                  .value["items"]
                  .size(),
              1);
    EXPECT_EQ(surface.call("list_reviews", {{"repository_id", imported.value["repository"]["id"]}})
                  .value["items"]
                  .size(),
              1);
}

TEST(Forge, ReleaseRetainsVerifiedArtifactAttestations) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "releases"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;

    const ApiResult release = surface.call("create_release",
                                           {{"repository_id", repository.value["id"]},
                                            {"tag", "v1.0.0"},
                                            {"change_id", "change-1"},
                                            {"artifact_ids", json::array({"result-1"})}});
    ASSERT_TRUE(release.ok) << release.error.message;
    EXPECT_EQ(release.value["value"]["artifacts"][0]["id"], "result-1");
    EXPECT_EQ(release.value["value"]["artifacts"][0]["attestations"][0]["id"], "attestation-1");
}

TEST(Forge, WebhookDeliveryRetriesAndRetainsAnEventLog) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "hooks"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    const ApiResult webhook = surface.call("create_webhook",
                                           {{"repository_id", repository_id},
                                            {"url", "https://example.test/lake"},
                                            {"events", json::array({"issue"})},
                                            {"secret", "delivery-secret"}});
    ASSERT_TRUE(webhook.ok) << webhook.error.message;

    int calls = 0;
    surface.ctx.webhook_sender = [&calls](const std::string&,
                                          const json& headers,
                                          const std::string&) -> WebhookDeliveryResult {
        EXPECT_TRUE(headers.at("x-lake-signature-256").get<std::string>().starts_with("sha256="));
        ++calls;
        if (calls == 1) {
            return {.status = 503, .error = "service unavailable"};
        }
        return {.status = 204};
    };
    ASSERT_TRUE(
        surface.call("create_issue", {{"repository_id", repository_id}, {"title", "Trigger hook"}})
            .ok);

    const ApiResult listed =
        surface.call("list_webhook_deliveries", {{"repository_id", repository_id}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1);
    EXPECT_FALSE(listed.value["items"][0]["value"]["delivered"]);
    const ApiResult retried = surface.call(
        "retry_webhook_delivery",
        {{"repository_id", repository_id}, {"delivery_id", listed.value["items"][0]["id"]}});
    ASSERT_TRUE(retried.ok) << retried.error.message;
    EXPECT_TRUE(retried.value["value"]["delivered"]);
    EXPECT_EQ(retried.value["value"]["attempt"], 2);
    EXPECT_EQ(calls, 2);
}

TEST(Forge, BranchProtectionRequiresAnApprovedReview) {
    Surface surface;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok) << user.error.message;
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok) << customer.error.message;
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "protected"}});
    ASSERT_TRUE(repository.ok) << repository.error.message;
    const std::string repository_id = repository.value["id"];
    ASSERT_TRUE(surface
                    .call("set_branch_protection",
                          {{"repository_id", repository_id},
                           {"ref", "refs/heads/main"},
                           {"required_review", true},
                           {"allowed_pushers", json::array({"admin"})}})
                    .ok);

    const BranchRefUpdate update{
        .name = "refs/heads/main", .new_head = "change-1", .fast_forward = true};
    const auto rejected = evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, repository_id, update);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "conflict");

    const ApiResult review = surface.call(
        "create_review", {{"repository_id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(review.ok) << review.error.message;
    ASSERT_TRUE(surface
                    .call("update_review",
                          {{"repository_id", repository_id},
                           {"review_id", review.value["id"]},
                           {"state", "approved"}})
                    .ok);
    EXPECT_TRUE(evaluate_branch_protection(
        surface.store, "admin", surface.ctx.authorization_cache, repository_id, update));
}

TEST(Repositories, AnonymousCallersOnlySeePublicRepositories) {
    Surface s;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult private_repository =
        s.call("create_repository",
               {{"customer_id", customer.value["id"]}, {"name", "private-repository"}});
    const ApiResult public_repository = s.call("create_repository",
                                               {{"customer_id", customer.value["id"]},
                                                {"name", "public-repository"},
                                                {"visibility", "public"}});
    ASSERT_TRUE(private_repository.ok);
    ASSERT_TRUE(public_repository.ok);

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    const ApiResult listed = s.call("list_repositories");
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1u);
    EXPECT_EQ(listed.value["items"][0]["id"], public_repository.value["id"]);
    EXPECT_TRUE(s.call("get_repository", {{"id", public_repository.value["id"]}}).ok);
    const ApiResult hidden = s.call("get_repository", {{"id", private_repository.value["id"]}});
    EXPECT_EQ(hidden.error.code, "unauthorized");

    for (const auto& capability : s.registry.list()) {
        if (capability->name.starts_with("list_")) {
            EXPECT_TRUE(capability->authorizes_candidates) << capability->name;
        }
    }
}

TEST(Repositories, PublicRepositoryContentCapabilitiesUseTheRepositoryStore) {
    Surface s;
    RepositoryFixtureStore repository_store;
    s.ctx.sync_store = &repository_store;
    const ApiResult user = s.call("create_user", {{"display_name", "Ada"}});
    ASSERT_TRUE(user.ok);
    const ApiResult customer =
        s.call("create_customer",
               {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = s.call("create_repository",
                                        {{"customer_id", customer.value["id"]},
                                         {"name", "public-repository"},
                                         {"visibility", "public"}});
    ASSERT_TRUE(repository.ok);
    const std::string repository_id = repository.value["id"];

    s.ctx.authorized = false;
    s.ctx.acting_identity.clear();
    EXPECT_EQ(s.call("repository_refs", {{"id", repository_id}}).value["items"][0]["name"], "main");
    EXPECT_EQ(s.call("repository_tree", {{"id", repository_id}, {"ref", "main"}})
                  .value["items"][0]["path"],
              "readme.md");
    EXPECT_EQ(
        s.call("repository_file", {{"id", repository_id}, {"ref", "main"}, {"path", "readme.md"}})
            .value["contents"],
        "new\nshared\n");
    EXPECT_EQ(s.call("list_changes", {{"id", repository_id}}).value["items"].size(), 1u);
    EXPECT_EQ(
        s.call("get_change", {{"id", repository_id}, {"change_id", "change-1"}}).value["message"],
        "initial");
    const ApiResult difference =
        s.call("change_diff", {{"id", repository_id}, {"change_id", "change-1"}});
    ASSERT_TRUE(difference.ok) << difference.error.message;
    EXPECT_EQ(difference.value["items"][0]["path"], "readme.md");
    EXPECT_EQ(difference.value["items"][0]["line_hunks"][0]["added_lines"][0], "hello");
    const ApiResult conflicts = s.call("list_conflicts", {{"id", repository_id}});
    ASSERT_TRUE(conflicts.ok) << conflicts.error.message;
    EXPECT_EQ(conflicts.value["items"][0]["region"]["path"], "readme.md");
    EXPECT_EQ(conflicts.value["items"][0]["kind"], "suspicious-merge");
    EXPECT_EQ(conflicts.value["items"][0]["explanation"], "Both sides changed the greeting.");
    EXPECT_EQ(conflicts.value["items"][0]["reasons"][0], "The edits choose different words.");
    const ApiResult artifacts = s.call("list_artifacts", {{"id", repository_id}, {"limit", 1}});
    ASSERT_TRUE(artifacts.ok) << artifacts.error.message;
    EXPECT_EQ(artifacts.value["items"][0]["id"], "result-1");
    EXPECT_TRUE(artifacts.value["done"]);
    const ApiResult artifact =
        s.call("get_artifact", {{"id", repository_id}, {"result_id", "result-1"}});
    ASSERT_TRUE(artifact.ok) << artifact.error.message;
    EXPECT_EQ(artifact.value["outputs"][0]["path"], "build/app");
    EXPECT_EQ(artifact.value["attestations"][0]["profile"], "slsa_provenance/1");
    EXPECT_EQ(artifact.value["trace"]["id"], "trace-1");
}

TEST(Drive, FolderSharingMatchesAuthorizeAndLinksExpire) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult owner = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:owner"}, {"display_name", "Owner"}});
    const ApiResult recipient = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:recipient"}, {"display_name", "Recipient"}});
    const ApiResult outsider = surface.call(
        "create_user", {{"taap_id", "taap:1:ed25519:outsider"}, {"display_name", "Outsider"}});
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(recipient.ok);
    ASSERT_TRUE(outsider.ok);
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "owner"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const auto repository_object =
        surface.store.find_policy_object("repository", repository.value["id"]);
    ASSERT_TRUE(repository_object);
    ASSERT_TRUE(surface.store.create_grant({"taap:1:ed25519:owner",
                                            repository_object->id,
                                            "admin",
                                            "attestation-owner",
                                            "admin",
                                            "request-owner"}));

    surface.ctx.operator_credential = false;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult shared = surface.call("share_drive_folder",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", "shared"},
                                           {"principal_taap_id", "taap:1:ed25519:recipient"},
                                           {"role", "read"},
                                           {"attestation_digest", "attestation-recipient"}});
    ASSERT_TRUE(shared.ok) << shared.error.message;
    const std::string folder_object = shared.value["object"]["id"];

    surface.ctx.acting_identity = "taap:1:ed25519:recipient";
    const ApiResult visible = surface.call(
        "drive_browse",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}});
    ASSERT_TRUE(visible.ok) << visible.error.message;
    ASSERT_EQ(visible.value["items"].size(), 1u);
    EXPECT_EQ(visible.value["items"][0]["path"], "shared/readme.md");

    const ApiResult shared_with_me = surface.call("drive_shared_with_me");
    ASSERT_TRUE(shared_with_me.ok) << shared_with_me.error.message;
    ASSERT_EQ(shared_with_me.value["items"].size(), 1u);
    EXPECT_EQ(shared_with_me.value["items"][0]["repository"]["id"], repository.value["id"]);
    EXPECT_EQ(shared_with_me.value["items"][0]["path"], "shared");
    EXPECT_EQ(shared_with_me.value["items"][0]["shared_by"], "taap:1:ed25519:owner");
    EXPECT_EQ(shared_with_me.value["items"][0]["actions"], json::array({"read"}));

    surface.ctx.acting_identity = "taap:1:ed25519:outsider";
    const ApiResult hidden = surface.call(
        "drive_browse",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}});
    EXPECT_EQ(hidden.error.code, "unauthorized");
    EXPECT_TRUE(surface.call("drive_shared_with_me").value["items"].empty());

    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult effective = surface.call(
        "drive_effective_access",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "shared"}});
    ASSERT_TRUE(effective.ok) << effective.error.message;
    for (const json& entry : effective.value["items"]) {
        for (const std::string action : {"read", "write", "share"}) {
            lake::control::AuthorizationCache cache;
            const auto decision = lake::control::authorize(
                surface.store, entry["principal_taap_id"], folder_object, action, cache);
            ASSERT_TRUE(decision);
            const auto actions = entry["actions"].get<std::vector<std::string>>();
            EXPECT_EQ(decision->allowed, std::ranges::find(actions, action) != actions.end());
        }
    }

    const ApiResult active_link = surface.call("create_drive_share_link",
                                               {{"repository_id", repository.value["id"]},
                                                {"ref", "main"},
                                                {"path", "shared"},
                                                {"role", "read"},
                                                {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(active_link.ok) << active_link.error.message;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    repository_store.shared_folder_moved = true;
    const ApiResult moved_link = surface.call(
        "resolve_drive_share_link", {{"token", active_link.value["token"]}, {"ref", "main"}});
    ASSERT_TRUE(moved_link.ok) << moved_link.error.message;
    EXPECT_EQ(moved_link.value["path"], "renamed");
    EXPECT_EQ(moved_link.value["items"][0]["path"], "renamed/readme.md");
    const json file_input{
        {"token", active_link.value["token"]}, {"ref", "main"}, {"path", "renamed/readme.md"}};
    const ApiResult preview = surface.call("preview_drive_share_link", file_input);
    ASSERT_TRUE(preview.ok) << preview.error.message;
    EXPECT_NE(preview.value["html"].get<std::string>().find("new"), std::string::npos);
    const ApiResult downloaded = surface.call("download_drive_share_link", file_input);
    ASSERT_TRUE(downloaded.ok) << downloaded.error.message;
    EXPECT_EQ(downloaded.value["content_base64"], base64_bytes(bytes("new\nshared\n")));
    EXPECT_EQ(surface.ctx.sync_response, bytes("new\nshared\n"));
    for (const std::string name :
         {"resolve_drive_share_link", "preview_drive_share_link", "download_drive_share_link"}) {
        for (const std::string path : {"",
                                       "readme.md",
                                       "shared/readme.md",
                                       "renamed-other/file",
                                       "renamed/../readme.md",
                                       "renamed//readme.md",
                                       "renamed/./readme.md",
                                       "/renamed/readme.md",
                                       "renamed/..",
                                       "renamed/.lake-folder"}) {
            // An operator session must not add authority to a bearer link either.
            surface.ctx.acting_identity = "admin";
            surface.ctx.authorized = true;
            const ApiResult denied = surface.call(
                name, {{"token", active_link.value["token"]}, {"ref", "main"}, {"path", path}});
            EXPECT_EQ(denied.error.code, "unauthorized") << name << " " << path;
            EXPECT_TRUE(surface.ctx.sync_response.empty());
        }
    }
    const json document = openapi_document(surface.registry, "0.0.1");
    const auto tools = mcp_dispatch(surface.registry,
                                    {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                    surface.ctx,
                                    "0.0.1");
    ASSERT_TRUE(tools);
    const json listed = json::parse(*tools)["result"]["tools"];
    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_TRUE(document["paths"].contains("/v1/" + name));
        const auto tool =
            std::ranges::find_if(listed, [&](const json& entry) { return entry["name"] == name; });
        ASSERT_NE(tool, listed.end());
        EXPECT_EQ(tool->at("inputSchema"), surface.registry.find(name)->input_schema);
        EXPECT_EQ(tool->at("outputSchema"), surface.registry.find(name)->output_schema);
    }
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    const auto mcp_download = mcp_dispatch(
        surface.registry,
        {{"jsonrpc", "2.0"},
         {"id", 2},
         {"method", "tools/call"},
         {"params", {{"name", "download_drive_share_link"}, {"arguments", file_input}}}},
        surface.ctx,
        "0.0.1");
    ASSERT_TRUE(mcp_download);
    EXPECT_EQ(json::parse(*mcp_download)["result"]["structuredContent"]["value"]["content_base64"],
              downloaded.value["content_base64"]);
    surface.ctx.authorized = true;
    surface.ctx.acting_identity = "taap:1:ed25519:owner";
    const ApiResult revoked =
        surface.call("revoke_drive_share_link", {{"link_id", active_link.value["id"]}});
    ASSERT_TRUE(revoked.ok) << revoked.error.message;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    EXPECT_EQ(surface
                  .call("resolve_drive_share_link",
                        {{"token", active_link.value["token"]}, {"ref", "main"}})
                  .error.code,
              "unauthorized");

    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_EQ(surface.call(name, file_input).error.code, "unauthorized");
        EXPECT_TRUE(surface.ctx.sync_response.empty());
    }

    const std::string expired_token = "expired-token";
    ASSERT_TRUE(surface.store.create_drive_share_link({repository.value["id"],
                                                       folder_object,
                                                       "22222222222222222222222222222222",
                                                       "shared",
                                                       "read",
                                                       share_token_hash(expired_token),
                                                       "2020-01-01T00:00:00Z",
                                                       "taap:1:ed25519:owner",
                                                       "request-expired"}));
    const ApiResult expired =
        surface.call("resolve_drive_share_link", {{"token", expired_token}, {"ref", "main"}});
    EXPECT_EQ(expired.error.code, "unauthorized");
    for (const std::string name : {"preview_drive_share_link", "download_drive_share_link"}) {
        EXPECT_EQ(
            surface
                .call(name,
                      {{"token", expired_token}, {"ref", "main"}, {"path", "renamed/readme.md"}})
                .error.code,
            "unauthorized");
        EXPECT_TRUE(surface.ctx.sync_response.empty());
    }
}

TEST(Drive, ShareDownloadsEncodeAcrossBlockBoundaries) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult owner = surface.call("create_user", {{"display_name", "Owner"}});
    ASSERT_TRUE(owner.ok);
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", owner.value["id"]}, {"name", "share-bytes"}});
    ASSERT_TRUE(customer.ok);
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "drive"}});
    ASSERT_TRUE(repository.ok);
    const ApiResult link = surface.call("create_drive_share_link",
                                        {{"repository_id", repository.value["id"]},
                                         {"ref", "main"},
                                         {"path", "shared"},
                                         {"role", "read"},
                                         {"expires_at", "2999-01-01T00:00:00Z"}});
    ASSERT_TRUE(link.ok) << link.error.message;
    std::string content(3 * 1024 * 1024 + 1, '\xff');
    content[3 * 1024 * 1024] = '\0';
    repository_store.blob_contents = content;
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    const ApiResult downloaded = surface.call(
        "download_drive_share_link",
        {{"token", link.value["token"]}, {"ref", "main"}, {"path", "shared/readme.md"}});
    ASSERT_TRUE(downloaded.ok) << downloaded.error.message;
    EXPECT_EQ(downloaded.value["size"], content.size());
    EXPECT_EQ(downloaded.value["content_base64"], base64_bytes(bytes(content)));
    EXPECT_EQ(surface.ctx.sync_response, bytes(content));
}

TEST(Drive, PathHistoryRestoresAsANewChange) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "history"}});
    ASSERT_TRUE(repository.ok);
    const ApiResult history = surface.call(
        "drive_history",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(history.ok) << history.error.message;
    ASSERT_EQ(history.value["items"].size(), 1u);
    EXPECT_EQ(history.value["items"][0]["id"], "change-1");
    const ApiResult restored = surface.call("restore_drive_version",
                                            {{"repository_id", repository.value["id"]},
                                             {"ref", "main"},
                                             {"path", "readme.md"},
                                             {"change_id", "change-1"}});
    ASSERT_TRUE(restored.ok) << restored.error.message;
    EXPECT_EQ(restored.value["id"], "restored-change");
    EXPECT_TRUE(repository_store.restored);
}

TEST(Drive, BrowseMutationsDownloadsDiffsAndConflictsUseRepositoryCapabilities) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call(
        "create_repository", {{"customer_id", customer.value["id"]}, {"name", "drive"}});
    ASSERT_TRUE(repository.ok);

    const ApiResult browse = surface.call("drive_browse",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", ""},
                                           {"sort", "modified"},
                                           {"direction", "desc"}});
    ASSERT_TRUE(browse.ok) << browse.error.message;
    EXPECT_EQ(browse.value["items"].front()["kind"], "tree");
    EXPECT_EQ(browse.value["items"][1]["modified_at"], "2026-09-04T00:00:00Z");

    const ApiResult folder = surface.call("create_drive_folder",
                                          {{"repository_id", repository.value["id"]},
                                           {"ref", "main"},
                                           {"path", ""},
                                           {"name", "notes"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    ASSERT_EQ(repository_store.committed_operations.size(), 1u);
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::Folder);
    EXPECT_EQ(repository_store.committed_operations.front().path, "notes");

    const ApiResult upload =
        surface.call("upload_drive_files",
                     {{"repository_id", repository.value["id"]},
                      {"ref", "main"},
                      {"path", "shared"},
                      {"files", json::array({{{"name", "note.txt"}, {"content", "aGVsbG8="}}})}});
    ASSERT_TRUE(upload.ok) << upload.error.message;
    ASSERT_EQ(repository_store.committed_operations.size(), 1u);
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::File);
    EXPECT_EQ(repository_store.committed_operations.front().path, "shared/note.txt");
    EXPECT_EQ(repository_store.committed_operations.front().contents, bytes("hello"));

    const ApiResult moved = surface.call("move_drive_path",
                                         {{"repository_id", repository.value["id"]},
                                          {"ref", "main"},
                                          {"path", "readme.md"},
                                          {"destination", "renamed.md"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::Moved);
    EXPECT_EQ(repository_store.committed_operations.front().source_path, "readme.md");
    EXPECT_EQ(repository_store.committed_operations.front().path, "renamed.md");

    const ApiResult deleted = surface.call(
        "delete_drive_path",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(repository_store.committed_operations.front().kind, RepositoryWriteKind::DeletedFile);

    const ApiResult download = surface.call(
        "download_drive_file",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(download.ok) << download.error.message;
    EXPECT_EQ(download.value["blob"], "blob-2");
    EXPECT_EQ(surface.ctx.sync_response, bytes("new\nshared\n"));

    const ApiResult difference = surface.call("diff_drive_versions",
                                              {{"repository_id", repository.value["id"]},
                                               {"ref", "main"},
                                               {"path", "readme.md"},
                                               {"left", "change-1"},
                                               {"right", "main"}});
    ASSERT_TRUE(difference.ok) << difference.error.message;
    EXPECT_EQ(difference.value["lines"][0]["kind"], "removed");
    EXPECT_EQ(difference.value["lines"][0]["text"], "old");
    EXPECT_EQ(difference.value["lines"][1]["kind"], "added");
    EXPECT_EQ(difference.value["lines"][1]["text"], "new");

    const ApiResult conflict = surface.call(
        "drive_conflict",
        {{"repository_id", repository.value["id"]}, {"ref", "main"}, {"path", "readme.md"}});
    ASSERT_TRUE(conflict.ok) << conflict.error.message;
    EXPECT_EQ(conflict.value["versions"].size(), 2u);
    EXPECT_EQ(conflict.value["difference"]["lines"][0]["kind"], "removed");
    EXPECT_EQ(conflict.value["conflict"]["kind"], "suspicious-merge");
    EXPECT_EQ(conflict.value["conflict"]["region"]["bytes"]["length"], 3u);

    const ApiResult resolved = surface.call("resolve_conflict",
                                            {{"repository_id", repository.value["id"]},
                                             {"ref", "main"},
                                             {"path", "readme.md"},
                                             {"conflict_id", "conflict-1"},
                                             {"resolution", "blob"},
                                             {"blob", "cmVzb2x2ZWQ="}});
    ASSERT_TRUE(resolved.ok) << resolved.error.message;
    EXPECT_EQ(resolved.value["conflict"], "conflict-1");
    EXPECT_EQ(resolved.value["value"], "blob-resolved");
    EXPECT_EQ(resolved.value["change"], "change-resolved");
    EXPECT_EQ(repository_store.resolved_conflict, "conflict-1");
    EXPECT_EQ(repository_store.conflict_resolution, "blob");
    EXPECT_EQ(repository_store.conflict_blob, bytes("resolved"));
}

TEST(Models, MetadataAndResolveUseRepositoryHistoryAndProvenance) {
    Surface surface;
    RepositoryFixtureStore repository_store;
    surface.ctx.sync_store = &repository_store;
    const ApiResult user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", user.value["id"]}, {"name", "ada"}});
    const ApiResult repository = surface.call("create_repository",
                                              {{"customer_id", customer.value["id"]},
                                               {"name", "fixture-model"},
                                               {"visibility", "public"}});
    ASSERT_TRUE(repository.ok);
    surface.ctx.authorized = false;
    surface.ctx.acting_identity.clear();
    const ApiResult model =
        surface.call("get_model", {{"id", repository.value["id"]}, {"revision", "main"}});
    ASSERT_TRUE(model.ok) << model.error.message;
    EXPECT_EQ(model.value["card"], "# Fixture model");
    EXPECT_TRUE(model.value["provenance_complete"]);
    EXPECT_TRUE(model.value["provenance"][0]["sbom"]);
    EXPECT_TRUE(model.value["provenance"][0]["slsa_provenance"]);
    const ApiResult file = surface.call(
        "resolve_model_file",
        {{"id", repository.value["id"]}, {"revision", "main"}, {"path", "weights.bin"}});
    ASSERT_TRUE(file.ok) << file.error.message;
    EXPECT_EQ(file.value["contents"], "fixture weights");
}

TEST(HuggingFaceHttp, CommonModelAndDatasetCallsUseRepositoryCapabilities) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user =
        control->create_user({"taap:1:ed25519:hf-owner", "hf-owner@example.test", "Hub Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "fixture-model", "Hub fixture", "public"});
    ASSERT_TRUE(repository) << repository.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "hub-test";
    options.admin_token = "hub-admin";
    options.instance_kek = "hub-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string authorization = "Bearer hub-admin";

    const std::string preupload = http_request(server.port(),
                                               "POST",
                                               "/api/models/ada/fixture-model/preupload/main",
                                               R"({"files":[{"path":"config.json","size":12}]})",
                                               "application/json",
                                               {},
                                               authorization);
    EXPECT_NE(preupload.find(" 200 "), std::string::npos) << preupload;
    EXPECT_EQ(response_json(preupload)["files"][0]["uploadMode"], "regular");

    const std::string config = "{\"hidden\":8}";
    const std::string commit_body = R"({"key":"header","value":{"summary":"initial model"}})"
                                    "\n" +
                                    nlohmann::json({{"key", "file"},
                                                    {"value",
                                                     {{"path", "config.json"},
                                                      {"content", base64_bytes(bytes(config))},
                                                      {"encoding", "base64"}}}})
                                        .dump() +
                                    "\n";
    const std::string committed = http_request(server.port(),
                                               "POST",
                                               "/api/models/ada/fixture-model/commit/main",
                                               commit_body,
                                               "application/x-ndjson",
                                               {},
                                               authorization);
    EXPECT_NE(committed.find(" 200 "), std::string::npos) << committed;
    const std::string revision = response_json(committed).value("commitOid", "");
    EXPECT_FALSE(revision.empty());

    const std::string info = http_request(server.port(),
                                          "GET",
                                          "/api/models/ada/fixture-model/revision/" + revision,
                                          "",
                                          "application/json");
    EXPECT_NE(info.find(" 200 "), std::string::npos) << info;
    EXPECT_EQ(response_json(info)["siblings"][0]["rfilename"], "config.json");
    const std::string dataset_info = http_request(
        server.port(), "GET", "/api/datasets/ada/fixture-model", "", "application/json");
    EXPECT_NE(dataset_info.find(" 200 "), std::string::npos) << dataset_info;
    const std::string tree = http_request(server.port(),
                                          "GET",
                                          "/api/models/ada/fixture-model/tree/main?recursive=true",
                                          "",
                                          "application/json");
    EXPECT_EQ(response_json(tree)[0]["path"], "config.json") << tree;
    const std::string download = http_request(server.port(),
                                              "GET",
                                              "/ada/fixture-model/resolve/main/config.json",
                                              "",
                                              "application/octet-stream");
    EXPECT_NE(download.find(config), std::string::npos) << download;
    const std::string head = http_request(server.port(),
                                          "HEAD",
                                          "/ada/fixture-model/resolve/main/config.json",
                                          "",
                                          "application/octet-stream");
    EXPECT_NE(head.find("X-Repo-Commit: " + revision), std::string::npos) << head;

    const std::string large(6U * 1024U * 1024U, 'w');
    const std::string oid = lake::control::identity_crypto::sha256_hex(large);
    const std::string batch = http_request(
        server.port(),
        "POST",
        "/ada/fixture-model.git/info/lfs/objects/batch",
        nlohmann::json(
            {{"operation", "upload"},
             {"transfers", nlohmann::json::array({"basic"})},
             {"objects", nlohmann::json::array({{{"oid", oid}, {"size", large.size()}}})}})
            .dump(),
        "application/vnd.git-lfs+json",
        {},
        authorization);
    EXPECT_NE(batch.find(" 200 "), std::string::npos) << batch;
    const std::string href = response_json(batch)["objects"][0]["actions"]["upload"]["href"];
    const std::size_t path_offset = href.find('/', href.find("//") + 2);
    ASSERT_NE(path_offset, std::string::npos);
    const std::string uploaded = http_request(
        server.port(), "PUT", href.substr(path_offset), large, "application/octet-stream");
    EXPECT_NE(uploaded.find(" 200 "), std::string::npos) << uploaded;
    const std::string lfs_commit =
        R"({"key":"header","value":{"summary":"weights"}})"
        "\n" +
        nlohmann::json({{"key", "lfsFile"},
                        {"value", {{"path", "weights.bin"}, {"oid", oid}, {"size", large.size()}}}})
            .dump() +
        "\n";
    const std::string lfs_committed = http_request(server.port(),
                                                   "POST",
                                                   "/api/models/ada/fixture-model/commit/main",
                                                   lfs_commit,
                                                   "application/x-ndjson",
                                                   {},
                                                   authorization);
    EXPECT_NE(lfs_committed.find(" 200 "), std::string::npos) << lfs_committed;
    const std::string model_page =
        http_request(server.port(), "GET", "/models/ada/fixture-model", "", "text/html");
    EXPECT_NE(model_page.find("Training provenance"), std::string::npos) << model_page;
    EXPECT_NE(model_page.find("weights.bin"), std::string::npos) << model_page;

    const std::string created = http_request(
        server.port(),
        "POST",
        "/api/repos/create",
        R"({"name":"new-dataset","organization":"ada","type":"dataset","private":false})",
        "application/json",
        {},
        authorization);
    EXPECT_NE(created.find(" 200 "), std::string::npos) << created;
    EXPECT_EQ(response_json(created)["repo_id"], "ada/new-dataset");
    const std::string yaml = http_request(server.port(),
                                          "POST",
                                          "/api/validate-yaml",
                                          R"({"content":"---\n---\n","repoType":"model"})",
                                          "application/json",
                                          {},
                                          authorization);
    EXPECT_NE(yaml.find(" 200 "), std::string::npos) << yaml;
    EXPECT_TRUE(response_json(yaml)["errors"].empty());
    server.stop();
}

TEST(HuggingFaceClient, CommonFlowsRunWhenPythonClientIsInstalled) {
    if (std::system("python3 -c 'import huggingface_hub' >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "huggingface_hub is not installed; direct HTTP coverage remains active";
    }
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:client-owner", "client-owner@example.test", "Client Owner"});
    ASSERT_TRUE(user);
    const auto customer = control->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer);
    ServerOptions options;
    options.port = 0;
    options.node_name = "huggingface-client-test";
    options.admin_token = "hub-admin";
    options.instance_kek = "huggingface-client-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::filesystem::path script = temporary.path() / "huggingface_client_test.py";
    {
        std::ofstream output(script);
        output << R"py(import os
import pathlib
import sys
from huggingface_hub import HfApi, hf_hub_download, snapshot_download

endpoint = sys.argv[1]
root = pathlib.Path(sys.argv[2])
api = HfApi(endpoint=endpoint, token="hub-admin")
api.create_repo("ada/client-model", repo_type="model", private=False)
api.upload_file(
    path_or_fileobj=b"model card",
    path_in_repo="README.md",
    repo_id="ada/client-model",
    repo_type="model",
)
folder = root / "folder"
folder.mkdir()
(folder / "config.json").write_text('{"layers": 2}')
(folder / "tokenizer.json").write_text('{"tokens": []}')
api.upload_folder(
    folder_path=folder,
    repo_id="ada/client-model",
    repo_type="model",
)
files = api.list_repo_files("ada/client-model", repo_type="model")
assert set(files) == {"README.md", "config.json", "tokenizer.json"}, files
info = api.model_info("ada/client-model")
assert info.sha
download = hf_hub_download(
    repo_id="ada/client-model",
    filename="config.json",
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(download).read_text() == '{"layers": 2}'
immutable = hf_hub_download(
    repo_id="ada/client-model",
    filename="tokenizer.json",
    revision=info.sha,
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(immutable).read_text() == '{"tokens": []}'
snapshot = snapshot_download(
    repo_id="ada/client-model",
    endpoint=endpoint,
    token="hub-admin",
)
assert (pathlib.Path(snapshot) / "README.md").read_bytes() == b"model card"
api.create_repo("ada/client-dataset", repo_type="dataset", private=False)
api.upload_file(
    path_or_fileobj=b'{"row": 1}\n',
    path_in_repo="data.jsonl",
    repo_id="ada/client-dataset",
    repo_type="dataset",
)
assert api.list_repo_files("ada/client-dataset", repo_type="dataset") == ["data.jsonl"]
dataset_download = hf_hub_download(
    repo_id="ada/client-dataset",
    filename="data.jsonl",
    repo_type="dataset",
    endpoint=endpoint,
    token="hub-admin",
)
assert pathlib.Path(dataset_download).read_bytes() == b'{"row": 1}\n'
dataset_snapshot = snapshot_download(
    repo_id="ada/client-dataset",
    repo_type="dataset",
    endpoint=endpoint,
    token="hub-admin",
)
assert (pathlib.Path(dataset_snapshot) / "data.jsonl").read_bytes() == b'{"row": 1}\n'
)py";
    }
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string command = "HF_HUB_DISABLE_XET=1 HF_HUB_DISABLE_TELEMETRY=1 "
                                "python3 '" +
                                script.string() + "' '" + endpoint + "' '" +
                                temporary.path().string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0);
    server.stop();
}

TEST(ModelProvenance, SignedDatasetChainVerifiesAndTamperingIsRefused) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user =
        control->create_user({"taap:1:ed25519:trainer", "trainer@example.test", "Trainer"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "training"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto dataset =
        control->create_repository({customer->id, "dataset", "Training data", "private"});
    const auto model =
        control->create_repository({customer->id, "model", "Trained model", "private"});
    ASSERT_TRUE(dataset);
    ASSERT_TRUE(model);

    auto dataset_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "dataset"));
    auto model_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "model"));
    ASSERT_TRUE(dataset_source);
    ASSERT_TRUE(model_source);
    ASSERT_TRUE(liquid::runtime::block_on(
        dataset_source->write("training.jsonl", bytes("{\"example\":1}\n"))));
    const auto dataset_change = liquid::runtime::block_on(dataset_source->cut("dataset"));
    ASSERT_TRUE(dataset_change);
    ASSERT_TRUE(
        liquid::runtime::block_on(model_source->write("weights.bin", bytes("trained weights"))));
    const auto model_change = liquid::runtime::block_on(model_source->cut("model"));
    ASSERT_TRUE(model_change);

    ServerOptions options;
    options.port = 0;
    options.node_name = "provenance-test";
    options.admin_token = "provenance-admin";
    options.instance_kek = "provenance-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    liquid::sync_endpoint dataset_endpoint(
        origin + "/v1/repositories/" + dataset->id,
        liquid::sync_endpoint_options{.bearer_token = "provenance-admin"});
    liquid::sync_endpoint model_endpoint(
        origin + "/v1/repositories/" + model->id,
        liquid::sync_endpoint_options{.bearer_token = "provenance-admin"});
    ASSERT_TRUE(liquid::runtime::block_on(dataset_endpoint.push(*dataset_source)));
    ASSERT_TRUE(liquid::runtime::block_on(model_endpoint.push(*model_source)));

    const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    liquid::attestation model_statement{
        .subject = model_change->value,
        .profile = "model_provenance/1",
        .at = at,
        .subject_kind = liquid::attestation_subject_kind::model,
        .attributes = {{"training_dataset", liquid::to_string(dataset_change->value)},
                       {"training_code", liquid::to_string(dataset_change->value)}}};
    model_statement = sign_attestation(std::move(model_statement));
    const liquid::slsa_resource_descriptor resource{
        "liquid:training", {{"blake3", liquid::to_string(dataset_change->value)}}, std::nullopt};
    liquid::attestation run_statement{
        .subject = model_change->value,
        .profile = "slsa_provenance/1",
        .at = at,
        .subject_kind = liquid::attestation_subject_kind::artifact,
        .slsa_provenance = liquid::slsa_provenance_v1{
            .build_definition = {.build_type = "https://lake.orama.com/training/v1",
                                 .external_parameters = {{"train", "--epochs", "3"},
                                                         "/workspace",
                                                         {{"CUDA", "12.8"}}},
                                 .internal_parameters = {.toolchain = resource,
                                                         .sandbox = resource,
                                                         .platform = {{"os", "linux"}}},
                                 .resolved_dependencies = {resource}},
            .run_details = {.builder = {"taap:1:ed25519:trainer"},
                            .metadata = {"run-7", "2026-09-05T00:00:00Z", "2026-09-05T00:01:00Z"},
                            .byproducts = {resource}}}};
    run_statement = sign_attestation(std::move(run_statement));
    const auto model_verification = liquid::verify_attestation(model_statement);
    const auto run_verification = liquid::verify_attestation(run_statement);
    ASSERT_TRUE(model_verification);
    ASSERT_TRUE(run_verification);
    EXPECT_TRUE(model_verification->verified);
    EXPECT_TRUE(run_verification->verified);
    const auto model_encoded = liquid::encode_attestation(model_statement);
    const auto run_encoded = liquid::encode_attestation(run_statement);
    ASSERT_TRUE(model_encoded);
    ASSERT_TRUE(run_encoded);

    const nlohmann::json request{{"id", model->id},
                                 {"revision", "main"},
                                 {"dataset_repository_id", dataset->id},
                                 {"dataset_revision", "main"},
                                 {"model_attestation", base64_bytes(*model_encoded)},
                                 {"run_attestation", base64_bytes(*run_encoded)}};
    const std::string attached = http_request(server.port(),
                                              "POST",
                                              "/v1/attach_model_provenance",
                                              request.dump(),
                                              "application/json",
                                              {},
                                              "Bearer provenance-admin");
    EXPECT_NE(attached.find(" 200 "), std::string::npos) << attached;
    EXPECT_TRUE(response_json(attached)["ok"]);
    EXPECT_TRUE(response_json(attached)["value"]["verified"]);
    const std::string verified =
        http_request(server.port(),
                     "POST",
                     "/v1/verify_model_provenance",
                     nlohmann::json({{"id", model->id}, {"revision", "main"}}).dump(),
                     "application/json",
                     {},
                     "Bearer provenance-admin");
    EXPECT_NE(verified.find(" 200 "), std::string::npos) << verified;
    EXPECT_TRUE(response_json(verified)["value"]["verified"]);
    EXPECT_EQ(response_json(verified)["value"]["dataset_revision"],
              liquid::to_string(dataset_change->value));
    const std::string model_page = http_request(server.port(),
                                                "GET",
                                                "/models/" + model->id,
                                                "",
                                                "text/html",
                                                {},
                                                "Bearer provenance-admin");
    EXPECT_NE(model_page.find("run-7"), std::string::npos) << model_page;
    EXPECT_NE(model_page.find(liquid::to_string(dataset_change->value)), std::string::npos)
        << model_page;

    std::vector<std::byte> tampered = *model_encoded;
    tampered.back() ^= std::byte{1};
    nlohmann::json tampered_request = request;
    tampered_request["model_attestation"] = base64_bytes(tampered);
    const std::string refused = http_request(server.port(),
                                             "POST",
                                             "/v1/attach_model_provenance",
                                             tampered_request.dump(),
                                             "application/json",
                                             {},
                                             "Bearer provenance-admin");
    EXPECT_NE(refused.find(" 400 "), std::string::npos) << refused;
    server.stop();
}

TEST(WebUiHttp, PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden) {
    TemporaryRepositoryRoot root;
    const auto store = std::make_shared<lake::control::InMemoryStore>();
    const auto user = store->create_user({"", "", "Ada"});
    ASSERT_TRUE(user);
    const auto customer = store->create_customer({"individual", user->id, "", "ada"});
    ASSERT_TRUE(customer);
    const auto public_repository =
        store->create_repository({customer->id, "public-repository", "visible", "public"});
    const auto private_repository =
        store->create_repository({customer->id, "private-repository", "hidden", "private"});
    const auto reformatting_repository =
        store->create_repository({customer->id, "reformatting-repository", "empty diff", "public"});
    ASSERT_TRUE(public_repository);
    ASSERT_TRUE(private_repository);
    ASSERT_TRUE(reformatting_repository);

    const std::filesystem::path repository_path =
        root.path() / customer->id / "repositories" / public_repository->id;
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root.path() / customer->id / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options repository_options;
    repository_options.store = &*customer_store;
    auto opened =
        liquid::runtime::block_on(liquid::repository::create(repository_path, repository_options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("readme.md", bytes("public content"))));
    const auto first_change = liquid::runtime::block_on(repository.cut("initial public content"));
    ASSERT_TRUE(first_change) << first_change.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("readme.md", bytes("public content\nupdated\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.md", bytes("new file\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("MODEL_CARD.md", bytes("# Recorded fixture model\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("weights.bin", bytes("recorded fixture weights"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("nested/config.json", bytes("{\"fixture\":true}"))));
    const auto second_change = liquid::runtime::block_on(repository.cut("update public content"));
    ASSERT_TRUE(second_change) << second_change.error().message;
    const std::vector<std::byte> artifact_bytes = bytes("artifact output");
    const auto artifact_content = liquid::hash(liquid::algorithm::blake3, artifact_bytes);
    ASSERT_TRUE(artifact_content) << artifact_content.error().message;
    const liquid::blob remote_artifact{
        .chunks = {*artifact_content},
        .size = artifact_bytes.size(),
        .chunk_sizes = {artifact_bytes.size()},
        .content = *artifact_content,
        .remote = liquid::remote_blob_location{"test", "worker-1"},
    };
    const auto artifact_record =
        liquid::runtime::block_on(repository.store_remote_blob(remote_artifact));
    ASSERT_TRUE(artifact_record) << artifact_record.error().message;
    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_FALSE(history->empty());
    const liquid::action_input build_input{.command = {"remote-build"},
                                           .outputs = {"build/app"},
                                           .inputs = history->front().root,
                                           .toolchain = history->front().root,
                                           .platform = host_platform()};
    auto first_result_value = liquid::action_result{
        .outputs = {{"build/app", 0100755, *artifact_record}},
        .stdout_output = *artifact_record,
        .stderr_output = *artifact_record,
        .timing = {.queued = std::chrono::milliseconds{1},
                   .sandbox = std::chrono::milliseconds{2},
                   .exec = std::chrono::milliseconds{3},
                   .hash = std::chrono::milliseconds{4},
                   .store = std::chrono::milliseconds{5}},
        .worker = "worker-1",
        .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}},
        .cache = liquid::cache_status::hit,
    };
    first_result_value.started_at = std::chrono::sys_seconds{std::chrono::seconds{98}};
    first_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{100}};
    const auto first_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, first_result_value));
    ASSERT_TRUE(first_result) << first_result.error().message;
    auto second_result_value = first_result_value;
    second_result_value.observed_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    second_result_value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{200}};
    const auto second_result = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, build_input, second_result_value));
    ASSERT_TRUE(second_result) << second_result.error().message;

    const std::filesystem::path reformatting_path =
        root.path() / customer->id / "repositories" / reformatting_repository->id;
    auto reformatting_opened = liquid::runtime::block_on(
        liquid::repository::create(reformatting_path, repository_options));
    ASSERT_TRUE(reformatting_opened) << reformatting_opened.error().message;
    auto reformatting_source = std::move(*reformatting_opened);
    ASSERT_TRUE(
        liquid::runtime::block_on(reformatting_source.write("format.cpp", bytes("int value;\n"))));
    const auto formatted_before =
        liquid::runtime::block_on(reformatting_source.cut("before formatting"));
    ASSERT_TRUE(formatted_before) << formatted_before.error().message;
    const auto formatting_change =
        liquid::runtime::block_on(reformatting_source.cut("reformatting"));
    ASSERT_TRUE(formatting_change) << formatting_change.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "test-node";
    options.admin_token = "test-token";
    options.store = store;
    options.store_root = root.path().string();
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();

    const std::string listed = http_get(server.port(), "/repositories");
    EXPECT_NE(listed.find(" 200 "), std::string::npos);
    EXPECT_NE(listed.find("public-repository"), std::string::npos);
    EXPECT_EQ(listed.find("private-repository"), std::string::npos);

    const std::string public_page =
        http_get(server.port(), "/repository?id=" + public_repository->id);
    EXPECT_NE(public_page.find(" 200 "), std::string::npos);
    EXPECT_NE(public_page.find("public-repository"), std::string::npos);
    const std::string tree_page =
        http_get(server.port(), "/repository/tree?id=" + public_repository->id + "&ref=main");
    EXPECT_NE(tree_page.find(" 200 "), std::string::npos);
    EXPECT_NE(tree_page.find("readme.md"), std::string::npos);
    const std::string file_page = http_get(
        server.port(), "/file?repository_id=" + public_repository->id + "&ref=main&path=readme.md");
    EXPECT_NE(file_page.find(" 200 "), std::string::npos);
    EXPECT_NE(file_page.find("public content"), std::string::npos);
    const std::string change_page = http_get(server.port(),
                                             "/change?repository_id=" + public_repository->id +
                                                 "&id=" + liquid::to_string(second_change->value));
    EXPECT_NE(change_page.find(" 200 "), std::string::npos);
    EXPECT_NE(change_page.find("readme.md"), std::string::npos);
    EXPECT_NE(change_page.find("notes.md"), std::string::npos);
    EXPECT_NE(change_page.find("@@ -2,0 +2,1 @@"), std::string::npos);
    EXPECT_NE(change_page.find("+updated"), std::string::npos);
    const std::string reformatting_page =
        http_get(server.port(),
                 "/change?repository_id=" + reformatting_repository->id +
                     "&id=" + liquid::to_string(formatting_change->value));
    EXPECT_NE(reformatting_page.find(" 200 "), std::string::npos);
    EXPECT_NE(reformatting_page.find("No changes to display."), std::string::npos);
    EXPECT_EQ(reformatting_page.find("<code>format.cpp</code>"), std::string::npos);
    const std::string artifacts_page =
        http_get(server.port(), "/artifacts?repository_id=" + public_repository->id + "&limit=1");
    EXPECT_NE(artifacts_page.find(" 200 "), std::string::npos);
    EXPECT_NE(artifacts_page.find("Build results"), std::string::npos);
    EXPECT_NE(artifacts_page.find("Next page"), std::string::npos);
    const std::string artifact_page =
        http_get(server.port(),
                 "/artifact?repository_id=" + public_repository->id +
                     "&id=" + liquid::to_string(second_result->value));
    EXPECT_NE(artifact_page.find(" 200 "), std::string::npos);
    EXPECT_NE(artifact_page.find("build/app"), std::string::npos);
    EXPECT_NE(artifact_page.find("slsa_provenance/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("sbom/1"), std::string::npos);
    EXPECT_NE(artifact_page.find("verified"), std::string::npos);

    const std::string model_response =
        http_get(server.port(), "/api/models/" + public_repository->id);
    EXPECT_NE(model_response.find(" 200 "), std::string::npos) << model_response;
    const json model = response_json(model_response);
    ASSERT_TRUE(model.is_object());
    EXPECT_EQ(model["card"], "# Recorded fixture model\n");
    EXPECT_TRUE(model["provenance_complete"]);
    const std::string model_file =
        http_get(server.port(), "/models/" + public_repository->id + "/resolve/main/weights.bin");
    EXPECT_NE(model_file.find(" 200 "), std::string::npos) << model_file;
    EXPECT_NE(model_file.find("recorded fixture weights"), std::string::npos);
    const std::string nested_model_file = http_get(
        server.port(), "/models/" + public_repository->id + "/resolve/main/nested/config.json");
    EXPECT_NE(nested_model_file.find(" 200 "), std::string::npos) << nested_model_file;
    EXPECT_NE(nested_model_file.find("{\"fixture\":true}"), std::string::npos);

    const std::string drive =
        http_get(server.port(),
                 "/drive?repository_id=" + public_repository->id + "&ref=main&path=&view=grid",
                 "Bearer test-token");
    EXPECT_NE(drive.find(" 200 "), std::string::npos) << drive;
    EXPECT_NE(drive.find("class=\"drive-grid\""), std::string::npos);
    EXPECT_NE(drive.find("action=\"/drive/upload\""), std::string::npos);
    EXPECT_EQ(drive.find(" · —"), std::string::npos);

    const std::string boundary = "lake-drive-boundary";
    const std::string multipart =
        "--" + boundary + "\r\nContent-Disposition: form-data; name=\"repository_id\"\r\n\r\n" +
        public_repository->id + "\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"ref\"\r\n\r\nmain\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"files\"; filename=\"browser.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\nbrowser upload\r\n--" +
        boundary + "--\r\n";
    const std::string uploaded = http_request(server.port(),
                                              "POST",
                                              "/drive/upload",
                                              multipart,
                                              "multipart/form-data; boundary=" + boundary,
                                              {},
                                              "Bearer test-token");
    EXPECT_NE(uploaded.find(" 302 "), std::string::npos) << uploaded;
    const std::string uploaded_preview = http_get(
        server.port(),
        "/drive/preview?repository_id=" + public_repository->id + "&ref=main&path=browser.txt",
        "Bearer test-token");
    EXPECT_NE(uploaded_preview.find(" 200 "), std::string::npos) << uploaded_preview;
    EXPECT_NE(uploaded_preview.find("browser upload"), std::string::npos);
    const std::string downloaded = http_get(
        server.port(),
        "/drive/download?repository_id=" + public_repository->id + "&ref=main&path=browser.txt",
        "Bearer test-token");
    EXPECT_NE(downloaded.find(" 200 "), std::string::npos) << downloaded;
    EXPECT_NE(downloaded.find("attachment; filename=\"browser.txt\""), std::string::npos)
        << downloaded;
    EXPECT_NE(downloaded.find("browser upload"), std::string::npos);
    const std::string version_diff =
        http_get(server.port(),
                 "/drive/history/diff?repository_id=" + public_repository->id +
                     "&ref=main&path=readme.md&left=" + liquid::to_string(first_change->value) +
                     "&right=" + liquid::to_string(second_change->value),
                 "Bearer test-token");
    EXPECT_NE(version_diff.find(" 200 "), std::string::npos) << version_diff;
    EXPECT_NE(version_diff.find("+updated"), std::string::npos);

    const std::string private_page =
        http_get(server.port(), "/repository?id=" + private_repository->id);
    EXPECT_NE(private_page.find(" 302 "), std::string::npos);
    const std::string private_change =
        http_get(server.port(), "/change?repository_id=" + private_repository->id + "&id=hidden");
    EXPECT_NE(private_change.find(" 302 "), std::string::npos);
    const std::string private_artifacts =
        http_get(server.port(), "/artifacts?repository_id=" + private_repository->id);
    EXPECT_NE(private_artifacts.find(" 302 "), std::string::npos);
    EXPECT_NE(private_page.find("Location: /login"), std::string::npos);
    const std::string private_api =
        http_get(server.port(), "/v1/get_repository?id=" + private_repository->id);
    EXPECT_NE(private_api.find(" 403 "), std::string::npos);
    server.stop();
}

TEST(Repositories, LifecycleUsesPolicyAndAuditRegistry) {
    Surface surface;
    const ApiResult first_user = surface.call("create_user", {{"display_name", "Ada"}});
    const ApiResult second_user = surface.call("create_user", {{"display_name", "Grace"}});
    ASSERT_TRUE(first_user.ok);
    ASSERT_TRUE(second_user.ok);
    const ApiResult first_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", first_user.value["id"]}, {"name", "ada"}});
    const ApiResult second_customer = surface.call(
        "create_customer",
        {{"customer_type", "individual"}, {"user_id", second_user.value["id"]}, {"name", "grace"}});
    ASSERT_TRUE(first_customer.ok);
    ASSERT_TRUE(second_customer.ok);
    const ApiResult created = surface.call(
        "create_repository", {{"customer_id", first_customer.value["id"]}, {"name", "liquid"}});
    ASSERT_TRUE(created.ok);
    const std::string repository_id = created.value["id"];

    ASSERT_TRUE(surface
                    .call("set_repository_visibility",
                          {{"id", repository_id}, {"visibility", "organization"}})
                    .ok);
    ASSERT_TRUE(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "liquid-next"}}).ok);
    const ApiResult transferred =
        surface.call("transfer_repository",
                     {{"id", repository_id}, {"customer_id", second_customer.value["id"]}});
    ASSERT_TRUE(transferred.ok);
    EXPECT_EQ(transferred.value["customer_id"], second_customer.value["id"]);
    const ApiResult deleted = surface.call("delete_repository", {{"id", repository_id}});
    ASSERT_TRUE(deleted.ok);
    EXPECT_FALSE(deleted.value["deleted_at"].is_null());
    EXPECT_FALSE(deleted.value["retention_until"].is_null());
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "again"}}).error.code,
        "conflict");

    const ApiResult events = surface.call("audit", {{"limit", 100}});
    ASSERT_TRUE(events.ok);
    EXPECT_TRUE(std::ranges::any_of(events.value["items"], [](const json& event) {
        return event["action"] == "delete_repository" && event["outcome"] == "success";
    }));

    surface.ctx.acting_identity = "guest";
    EXPECT_EQ(
        surface.call("rename_repository", {{"id", repository_id}, {"name", "denied"}}).error.code,
        "unauthorized");
}

TEST(ModelSync, MultiGigabyteRevisionsDeduplicateAndResumePull) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:model-owner", "model-owner@example.test", "Model Owner"});
    ASSERT_TRUE(user);
    const auto customer = control->create_customer({"individual", user->id, "", "models"});
    ASSERT_TRUE(customer);
    const auto repository =
        control->create_repository({customer->id, "large-model", "Large model", "private"});
    ASSERT_TRUE(repository);

    const std::filesystem::path weights = temporary.path() / "weights.bin";
    constexpr std::uint64_t logical_size = std::uint64_t{2} * 1024 * 1024 * 1024;
    {
        std::ofstream output(weights, std::ios::binary);
        ASSERT_TRUE(output);
    }
    std::filesystem::resize_file(weights, logical_size);
    const std::string block(1024 * 1024, 'm');
    {
        std::fstream file(weights, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file);
        file.seekp(64 * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
        file.seekp(std::streamoff{1024} * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "source-large"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(liquid::runtime::block_on(source->write_file("weights.bin", weights)));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("model revision one")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "model-sync-test";
    options.admin_token = "model-sync-admin";
    options.instance_kek = "model-sync-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());
    liquid::sync_endpoint endpoint(
        origin + "/v1/repositories/" + repository->id,
        liquid::sync_endpoint_options{.bearer_token = "model-sync-admin"});
    const auto first_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    const std::filesystem::path packs =
        temporary.path() / "lake" / customer->id / "store" / "packs";
    const std::uint64_t first_unique = unique_pack_raw_bytes(packs);

    {
        std::fstream file(weights, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file);
        const std::string zeros(block.size(), '\0');
        file.seekp(64 * 1024 * 1024);
        file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        file.seekp(68 * 1024 * 1024);
        file.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    ASSERT_TRUE(liquid::runtime::block_on(source->write_file("weights.bin", weights)));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("model revision two")));
    const auto second_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second_push) << second_push.error().message;
    const std::uint64_t second_unique = unique_pack_raw_bytes(packs);
    const std::uint64_t logical_revisions = logical_size * 2;
    const double dedup_ratio =
        1.0 - static_cast<double>(second_unique) / static_cast<double>(logical_revisions);
    std::cout << "multi-gigabyte model dedup ratio: " << dedup_ratio << '\n';
    EXPECT_GT(dedup_ratio, 0.99);
    EXPECT_LT(second_unique - first_unique, 16U * 1024U * 1024U);
    EXPECT_LT(second_push->sent.bytes, first_push->sent.bytes);

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "destination-large"));
    ASSERT_TRUE(destination);
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto resumed = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_EQ(resumed->received.bytes, 0U);
    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree);
    ASSERT_TRUE(destination_tree);
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));

    const std::string info = http_request(server.port(),
                                          "GET",
                                          "/api/models/" + repository->id,
                                          "",
                                          "application/json",
                                          {},
                                          "Bearer model-sync-admin");
    EXPECT_NE(info.find(" 200 "), std::string::npos) << info;
    const nlohmann::json storage = response_json(info).at("storage");
    EXPECT_EQ(storage["logical_bytes"], logical_revisions);
    EXPECT_GT(storage["dedup_ratio"].get<double>(), 0.99);
    EXPECT_EQ(storage["revisions"].size(), 2U);
    server.stop();
}

TEST(SyncHttp, PushPullShareCustomerStoreAndTransferOnlyMissingChunks) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:sync-owner", "sync-owner@example.test", "Sync Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "sync-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "project", "sync fixture", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto bad_repository =
        control->create_repository({customer->id, "bad-signature", "", "private"});
    ASSERT_TRUE(bad_repository) << bad_repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string first_contents(128 * 1024, 'a');
    ASSERT_TRUE(liquid::runtime::block_on(source->write("first.txt", sync_bytes(first_contents))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("first")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-test";
    options.admin_token = "sync-admin";
    options.instance_kek = "sync-test-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string direct_url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    RecordingProxy proxy(server.port());
    ASSERT_NE(proxy.port(), 0);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(proxy.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto first_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    EXPECT_GT(first_push->sent.bytes, 0U);

    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("second.txt", sync_bytes("second version"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("second")));
    const auto second_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(second_push) << second_push.error().message;
    EXPECT_GT(second_push->sent.bytes, 0U);
    EXPECT_LT(second_push->sent.bytes, first_push->sent.bytes);
    const auto unchanged_push = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(unchanged_push) << unchanged_push.error().message;
    EXPECT_EQ(unchanged_push->sent.bytes, 0U);
    proxy.stop();

    auto corrupt_push = proxy.push_body();
    ASSERT_FALSE(corrupt_push.empty());
    auto page = std::vector<liquid::sync_item>(256);
    const auto exported =
        liquid::runtime::block_on(source->export_drips(liquid::journal_cursor{}, page));
    ASSERT_TRUE(exported) << exported.error().message;
    const auto signed_drip = std::ranges::find_if(
        page.begin(), page.begin() + exported->count, [](const liquid::sync_item& item) {
            return item.kind == liquid::sync_item_kind::drip && !item.signature.empty();
        });
    ASSERT_NE(signed_drip, page.begin() + exported->count);
    const auto signature = std::search(corrupt_push.begin(),
                                       corrupt_push.end(),
                                       signed_drip->signature.begin(),
                                       signed_drip->signature.end());
    ASSERT_NE(signature, corrupt_push.end());
    *signature ^= std::byte{1};
    const std::string corrupt_body(reinterpret_cast<const char*>(corrupt_push.data()),
                                   corrupt_push.size());
    const std::string rejected =
        http_request(server.port(),
                     "POST",
                     "/v1/repositories/" + bad_repository->id + "/sync/push",
                     corrupt_body,
                     "application/vnd.liquid.sync-v1",
                     {},
                     "Bearer sync-admin");
    EXPECT_NE(rejected.find(" 400 "), std::string::npos) << rejected;

    auto destination =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    liquid::sync_endpoint pull_endpoint(
        direct_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto pulled = liquid::runtime::block_on(pull_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto source_first = liquid::runtime::block_on(source->read("first.txt"));
    const auto destination_first = liquid::runtime::block_on(destination->read("first.txt"));
    ASSERT_TRUE(source_first) << source_first.error().message;
    ASSERT_TRUE(destination_first) << destination_first.error().message;
    EXPECT_EQ(*destination_first, *source_first);
    const auto source_second = liquid::runtime::block_on(source->read("second.txt"));
    const auto destination_second = liquid::runtime::block_on(destination->read("second.txt"));
    ASSERT_TRUE(source_second) << source_second.error().message;
    ASSERT_TRUE(destination_second) << destination_second.error().message;
    EXPECT_EQ(*destination_second, *source_second);
    auto rejected_destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "rejected-destination"));
    ASSERT_TRUE(rejected_destination) << rejected_destination.error().message;
    const std::string rejected_url = "http://127.0.0.1:" + std::to_string(server.port()) +
                                     "/v1/repositories/" + bad_repository->id;
    liquid::sync_endpoint rejected_endpoint(
        rejected_url, liquid::sync_endpoint_options{.bearer_token = "sync-admin"});
    const auto rejected_pull =
        liquid::runtime::block_on(rejected_endpoint.pull(*rejected_destination));
    ASSERT_TRUE(rejected_pull) << rejected_pull.error().message;
    const auto rejected_tree = liquid::runtime::block_on(rejected_destination->root());
    ASSERT_TRUE(rejected_tree) << rejected_tree.error().message;
    EXPECT_TRUE(rejected_tree->entries.empty());
    EXPECT_FALSE(liquid::runtime::block_on(rejected_destination->head()));
    server.stop();

    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store"));
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "store" /
                                        "repositories"));
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "lake" / customer->id / "repositories" /
                                         repository->id / ".liquid-store"));
    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_FALSE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push_change" && event.draft.outcome == "success";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" && event.draft.outcome == "failure";
    }));
}

TEST(SyncHttp, TwoLakeInstancesReplicateRepositoryAndResumeFromCursor) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    auto control = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = control->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const std::string unique = lake::control::new_uuid_v4();
    const auto user = control->create_user(
        {"taap:1:ed25519:" + unique, unique + "@example.test", "Replication Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        control->create_customer({"individual", user->id, "", "replication-owner-" + unique});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "replicated", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    auto local_source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "local-source"));
    ASSERT_TRUE(local_source) << local_source.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(local_source->write("entry.txt", sync_bytes("region data"))));
    ASSERT_TRUE(liquid::runtime::block_on(local_source->cut("initial")));

    const unsigned short source_port = unused_loopback_port();
    const unsigned short replica_port = unused_loopback_port();
    ASSERT_NE(source_port, 0);
    ASSERT_NE(replica_port, 0);
    ASSERT_NE(source_port, replica_port);
    LakeProcess source(source_port, "region-a", database_uri, temporary.path() / "region-a");
    ASSERT_TRUE(source.ready());
    const std::string source_repository_url =
        "http://127.0.0.1:" + std::to_string(source_port) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint source_endpoint(
        source_repository_url, liquid::sync_endpoint_options{.bearer_token = "replication-admin"});
    const auto pushed = liquid::runtime::block_on(source_endpoint.push(*local_source));
    ASSERT_TRUE(pushed) << pushed.error().message;

    LakeProcess replica(replica_port, "region-b", database_uri, temporary.path() / "region-b");
    ASSERT_TRUE(replica.ready());

    const std::string source_origin = "http://127.0.0.1:" + std::to_string(source_port);
    const auto replicate = [&] {
        return http_request(replica_port,
                            "POST",
                            "/v1/replicate_repository",
                            json{{"repository_id", repository->id},
                                 {"source_instance", "region-a"},
                                 {"source_url", source_origin}}
                                .dump(),
                            "application/json",
                            {},
                            "Bearer replication-admin");
    };
    const std::string first_response = replicate();
    EXPECT_NE(first_response.find(" 200 "), std::string::npos) << first_response;
    EXPECT_GT(response_json(first_response).at("value").at("received_objects").get<std::uint64_t>(),
              0U);

    const std::string replica_repository_url =
        "http://127.0.0.1:" + std::to_string(replica_port) + "/v1/repositories/" + repository->id;
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    liquid::sync_endpoint replica_endpoint(
        replica_repository_url, liquid::sync_endpoint_options{.bearer_token = "replication-admin"});
    const auto pulled = liquid::runtime::block_on(replica_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto source_tree = liquid::runtime::block_on(local_source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));

    const std::string resumed_response = replicate();
    EXPECT_NE(resumed_response.find(" 200 "), std::string::npos) << resumed_response;
    EXPECT_EQ(response_json(resumed_response).at("value").at("received_bytes"), 0);
    EXPECT_TRUE(std::filesystem::exists(temporary.path() / "region-b" / customer->id /
                                        "repositories" / repository->id / "replication" /
                                        "region-a.cursor"));
}

TEST(SyncHttp, StorageQuotaThrottlesNewWritesAndKeepsExistingContentReadable) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto user = control->create_user(
        {"taap:1:ed25519:quota-owner", "quota-owner@example.test", "Quota Owner"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer = control->create_customer({"individual", user->id, "", "quota-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "quota-project", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "quota-source"));
    ASSERT_TRUE(source) << source.error().message;
    const std::string original(32 * 1024, 'a');
    ASSERT_TRUE(liquid::runtime::block_on(source->write("original.txt", sync_bytes(original))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("original")));

    ServerOptions options;
    options.port = 0;
    options.admin_token = "quota-admin";
    options.instance_kek = "quota-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer initial(options);
    ASSERT_TRUE(initial.start()) << initial.reason();
    const std::string initial_url =
        "http://127.0.0.1:" + std::to_string(initial.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint initial_endpoint(
        initial_url, liquid::sync_endpoint_options{.bearer_token = "quota-admin"});
    const auto first_push = liquid::runtime::block_on(initial_endpoint.push(*source));
    ASSERT_TRUE(first_push) << first_push.error().message;
    initial.stop();

    ASSERT_TRUE(liquid::runtime::block_on(
        source->write("refused.txt", sync_bytes(std::string(32 * 1024, 'b')))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("refused")));
    options.metering.storage_quota_bytes = 1;
    ApiServer limited(options);
    ASSERT_TRUE(limited.start()) << limited.reason();
    const std::string limited_url =
        "http://127.0.0.1:" + std::to_string(limited.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint limited_endpoint(
        limited_url, liquid::sync_endpoint_options{.bearer_token = "quota-admin"});
    const auto refused = liquid::runtime::block_on(limited_endpoint.push(*source));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, liquid::errc::busy);

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "quota-destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    const auto pulled = liquid::runtime::block_on(limited_endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const auto retained = liquid::runtime::block_on(destination->read("original.txt"));
    ASSERT_TRUE(retained) << retained.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(retained->data()), retained->size()),
              original);
    EXPECT_FALSE(liquid::runtime::block_on(destination->read("refused.txt")));
    limited.stop();
}

TEST(SyncHttp, PushWithoutWriteAccessIsRejectedAndAudited) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = control->create_user(
        {"taap:1:ed25519:sync-owner-denial", "owner-denial@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "sync-denial-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository =
        control->create_repository({customer->id, "private-project", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    const auto reader = control->sign_up_password(
        {"lake", "reader@example.test", "correct horse battery staple", "Reader", "test", "{}"});
    ASSERT_TRUE(reader) << reader.error().message;
    const auto principal = control->context_for(reader->secret);
    ASSERT_TRUE(principal) << principal.error().message;
    const auto object = control->find_policy_object("repository", repository->id);
    ASSERT_TRUE(object) << object.error().message;
    const auto grant = control->create_grant({principal->taap_id,
                                              object->id,
                                              "read",
                                              "attestation:sync-reader",
                                              owner->taap_id,
                                              "sync-denial"});
    ASSERT_TRUE(grant) << grant.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "denied-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(source->write("private.txt", sync_bytes("not authorized"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("denied")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "sync-denial";
    options.instance_kek = "sync-denial-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(url,
                                   liquid::sync_endpoint_options{.bearer_token = reader->secret});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    EXPECT_FALSE(pushed);
    ASSERT_FALSE(pushed.has_value());
    EXPECT_EQ(pushed.error().code, liquid::errc::unauthorized);
    server.stop();

    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_TRUE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.outcome == "denied" &&
               (event.draft.action == "upload_chunks" || event.draft.action == "push");
    }));
}

TEST(PostgresSync, PushAndPullUsePostgresControlPlane) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    std::shared_ptr<lake::control::Store> control = lake::control::make_postgres_store(
        database_uri, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    const auto ready = control->ping();
    ASSERT_TRUE(ready) << ready.error().message;
    const std::string suffix = lake::control::new_uuid_v4();
    const auto user = control->create_user({"taap:1:ed25519:postgres-sync-" + suffix,
                                            "postgres-sync-" + suffix + "@example.test",
                                            "Postgres Sync"});
    ASSERT_TRUE(user) << user.error().message;
    const auto customer =
        control->create_customer({"individual", user->id, "", "postgres-sync-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "project", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;

    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "postgres-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        source->write("postgres.txt", sync_bytes("postgres-backed sync"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("postgres")));

    ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-sync";
    options.admin_token = "postgres-sync-admin";
    options.instance_kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = "postgres-sync-admin"});
    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;

    auto destination = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "postgres-destination"));
    ASSERT_TRUE(destination) << destination.error().message;
    const auto pulled = liquid::runtime::block_on(endpoint.pull(*destination));
    ASSERT_TRUE(pulled) << pulled.error().message;
    const std::string metering_response = http_get(server.port(),
                                                   "/v1/metering_usage?customer_id=" + customer->id,
                                                   "Bearer postgres-sync-admin");
    EXPECT_NE(metering_response.find(" 200 "), std::string::npos) << metering_response;
    const json metering = response_json(metering_response);
    ASSERT_TRUE(metering.value("ok", false)) << metering.dump();
    ASSERT_FALSE(metering.at("value").at("items").empty());
    const std::uint64_t accounted =
        unique_pack_raw_bytes(temporary.path() / "lake" / customer->id / "store" / "packs");
    EXPECT_GT(accounted, 0U);
    EXPECT_EQ(metering.at("value").at("items").back().at("stored_bytes").get<std::uint64_t>(),
              accounted);
    EXPECT_GT(metering.at("value").at("items").back().at("transfer_in_bytes").get<std::uint64_t>(),
              0U);
    EXPECT_GT(metering.at("value").at("items").back().at("transfer_out_bytes").get<std::uint64_t>(),
              0U);
    server.stop();

    const auto source_tree = liquid::runtime::block_on(source->root());
    const auto destination_tree = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(source_tree) << source_tree.error().message;
    ASSERT_TRUE(destination_tree) << destination_tree.error().message;
    EXPECT_EQ(tree_fingerprint(*source_tree), tree_fingerprint(*destination_tree));
    const auto contents = liquid::runtime::block_on(destination->read("postgres.txt"));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(*contents, sync_bytes("postgres-backed sync"));

    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_FALSE(references->empty());
    const auto events = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events) << events.error().message;
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "push" && event.draft.outcome == "success";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events, [](const auto& event) {
        return event.draft.action == "pull" && event.draft.outcome == "success";
    }));
}

TEST(PostgresSync, EnforcesBranchProtectionAtReferenceUpdates) {
    const char* database_uri = std::getenv("LAKE_TEST_DB_URI");
    if (database_uri == nullptr || *database_uri == '\0') {
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    }
    TemporarySyncRoot temporary;
    const std::string instance_kek =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    std::shared_ptr<lake::control::Store> control =
        lake::control::make_postgres_store(database_uri, instance_kek);
    ASSERT_TRUE(control->ping());
    const std::string suffix = lake::control::new_uuid_v4();
    const auto owner = control->create_user({"taap:1:ed25519:branch-owner-" + suffix,
                                             "branch-owner-" + suffix + "@example.test",
                                             "Branch Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer =
        control->create_customer({"individual", owner->id, "", "branch-owner-" + suffix});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "protected", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;

    auto source = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "branch-protection-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.node_name = "postgres-branch-protection";
    options.admin_token = "postgres-branch-protection-admin";
    options.instance_kek = instance_kek;
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint owner_endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    ASSERT_TRUE(liquid::runtime::block_on(owner_endpoint.push(*source)));

    ASSERT_TRUE(liquid::runtime::block_on(source->write("reviewed.txt", sync_bytes("reviewed"))));
    const auto reviewed = liquid::runtime::block_on(source->cut("reviewed"));
    ASSERT_TRUE(reviewed) << reviewed.error().message;
    const std::string reviewed_id = liquid::to_string(reviewed->value);
    const auto protection =
        control->create_forge_record({repository->id,
                                      "branch_protection",
                                      {{"ref", "main"},
                                       {"required_review", true},
                                       {"fast_forward_only", true},
                                       {"allowed_pushers", json::array({"admin"})}}});
    ASSERT_TRUE(protection) << protection.error().message;

    const auto review_push = liquid::runtime::block_on(owner_endpoint.push(*source));
    ASSERT_TRUE(review_push) << review_push.error().message;
    const auto* review_refusal = reference_outcome(*review_push, "main");
    ASSERT_NE(review_refusal, nullptr);
    EXPECT_EQ(review_refusal->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(review_refusal->reason,
              "branch protection rejects main: an approved review is required");

    const auto approved = control->create_forge_record(
        {repository->id, "review", {{"change_id", reviewed_id}, {"state", "approved"}}});
    ASSERT_TRUE(approved) << approved.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(owner_endpoint.push(*source)));

    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("main", *base)));
    const auto fast_forward_push = liquid::runtime::block_on(owner_endpoint.push(*source));
    ASSERT_TRUE(fast_forward_push) << fast_forward_push.error().message;
    const auto* fast_forward_refusal = reference_outcome(*fast_forward_push, "main");
    ASSERT_NE(fast_forward_refusal, nullptr);
    EXPECT_EQ(fast_forward_refusal->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(fast_forward_refusal->reason,
              "branch protection rejects a non-fast-forward update to main");

    const auto events_before_unauthorized = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events_before_unauthorized) << events_before_unauthorized.error().message;
    const auto branch_events_before =
        std::ranges::count_if(*events_before_unauthorized, [](const auto& event) {
            return event.draft.kind.starts_with("forge.branch_protection.");
        });
    EXPECT_TRUE(std::ranges::any_of(*events_before_unauthorized, [](const auto& event) {
        return event.draft.kind == "forge.branch_protection.refused" &&
               event.draft.details.value("reference", "") == "main" &&
               event.draft.details.value("caller", "") == "admin" &&
               event.draft.details.value("rule", "") == "required_review";
    }));
    EXPECT_TRUE(std::ranges::any_of(*events_before_unauthorized, [](const auto& event) {
        return event.draft.kind == "forge.branch_protection.allowed" &&
               event.draft.details.value("reference", "") == "main" &&
               event.draft.details.value("caller", "") == "admin";
    }));

    auto anonymous_source = liquid::runtime::block_on(
        liquid::repository::create(temporary.path() / "branch-protection-anonymous"));
    ASSERT_TRUE(anonymous_source) << anonymous_source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        anonymous_source->write("anonymous.txt", sync_bytes("anonymous"))));
    ASSERT_TRUE(liquid::runtime::block_on(anonymous_source->cut("anonymous")));
    liquid::sync_endpoint anonymous_endpoint(url);
    const auto anonymous = liquid::runtime::block_on(anonymous_endpoint.push(*anonymous_source));
    ASSERT_FALSE(anonymous);
    EXPECT_EQ(anonymous.error().code, liquid::errc::unauthorized);
    const auto events_after_anonymous = control->list_audit_events({.limit = 1000});
    ASSERT_TRUE(events_after_anonymous) << events_after_anonymous.error().message;
    const auto branch_events_after =
        std::ranges::count_if(*events_after_anonymous, [](const auto& event) {
            return event.draft.kind.starts_with("forge.branch_protection.");
        });
    EXPECT_EQ(branch_events_after, branch_events_before);
    server.stop();
}

TEST(SyncHttp, ProtectedRefusalDoesNotBlockAnUnprotectedReference) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner =
        control->create_user({"taap:1:ed25519:mixed-branch-owner", "mixed@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer = control->create_customer({"individual", owner->id, "", "mixed-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "mixed", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "mixed-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.admin_token = "mixed-admin";
    options.instance_kek = "mixed-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token});
    ASSERT_TRUE(liquid::runtime::block_on(endpoint.push(*source)));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("candidate.txt", sync_bytes("candidate"))));
    const auto candidate = liquid::runtime::block_on(source->cut("candidate"));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("release", *candidate)));
    ASSERT_TRUE(control->create_forge_record(
        {repository->id, "branch_protection", {{"ref", "main"}, {"required_review", true}}}));

    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    ASSERT_EQ(pushed->references.size(), 2U);
    const auto* main_outcome = reference_outcome(*pushed, "main");
    const auto* release_outcome = reference_outcome(*pushed, "release");
    ASSERT_NE(main_outcome, nullptr);
    ASSERT_NE(release_outcome, nullptr);
    EXPECT_EQ(main_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(main_outcome->reason,
              "branch protection rejects main: an approved review is required");
    EXPECT_EQ(release_outcome->outcome, liquid::sync_ref_outcome_kind::applied);
    EXPECT_TRUE(release_outcome->reason.empty());
    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    EXPECT_TRUE(std::ranges::any_of(*references, [&base](const auto& reference) {
        return reference.name == "main" && reference.head == liquid::to_string(base->value);
    }));
    EXPECT_TRUE(std::ranges::any_of(*references, [&candidate](const auto& reference) {
        return reference.name == "release" && reference.head == liquid::to_string(candidate->value);
    }));
    server.stop();
}

TEST(SyncHttp, AtomicProtectedRefusalReportsEveryReferenceAndLandsNothing) {
    TemporarySyncRoot temporary;
    auto control = std::make_shared<lake::control::InMemoryStore>();
    const auto owner = control->create_user(
        {"taap:1:ed25519:atomic-branch-owner", "atomic@example.test", "Owner"});
    ASSERT_TRUE(owner) << owner.error().message;
    const auto customer = control->create_customer({"individual", owner->id, "", "atomic-owner"});
    ASSERT_TRUE(customer) << customer.error().message;
    const auto repository = control->create_repository({customer->id, "atomic", "", "private"});
    ASSERT_TRUE(repository) << repository.error().message;
    auto source =
        liquid::runtime::block_on(liquid::repository::create(temporary.path() / "atomic-source"));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("base.txt", sync_bytes("base"))));
    const auto base = liquid::runtime::block_on(source->cut("base"));
    ASSERT_TRUE(base) << base.error().message;

    ServerOptions options;
    options.port = 0;
    options.admin_token = "atomic-admin";
    options.instance_kek = "atomic-instance-kek";
    options.store_root = (temporary.path() / "lake").string();
    options.store = control;
    ApiServer server(options);
    ASSERT_TRUE(server.start()) << server.reason();
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/repositories/" + repository->id;
    liquid::sync_endpoint endpoint(
        url, liquid::sync_endpoint_options{.bearer_token = options.admin_token, .atomic = true});
    ASSERT_TRUE(liquid::runtime::block_on(endpoint.push(*source)));
    ASSERT_TRUE(liquid::runtime::block_on(source->write("candidate.txt", sync_bytes("candidate"))));
    const auto candidate = liquid::runtime::block_on(source->cut("candidate"));
    ASSERT_TRUE(candidate) << candidate.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->set_ref("release", *candidate)));
    ASSERT_TRUE(control->create_forge_record(
        {repository->id, "branch_protection", {{"ref", "main"}, {"required_review", true}}}));

    const auto pushed = liquid::runtime::block_on(endpoint.push(*source));
    ASSERT_TRUE(pushed) << pushed.error().message;
    ASSERT_EQ(pushed->references.size(), 2U);
    const auto* main_outcome = reference_outcome(*pushed, "main");
    const auto* release_outcome = reference_outcome(*pushed, "release");
    ASSERT_NE(main_outcome, nullptr);
    ASSERT_NE(release_outcome, nullptr);
    EXPECT_EQ(main_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(main_outcome->reason,
              "branch protection rejects main: an approved review is required");
    EXPECT_EQ(release_outcome->outcome, liquid::sync_ref_outcome_kind::refused);
    EXPECT_EQ(release_outcome->reason, "atomic push refused because another reference was refused");
    const auto references = control->list_repository_refs(repository->id);
    ASSERT_TRUE(references) << references.error().message;
    ASSERT_EQ(references->size(), 1U);
    EXPECT_EQ(references->front().name, "main");
    EXPECT_EQ(references->front().head, liquid::to_string(base->value));
    server.stop();
}

TEST(Identity, PasswordSessionsTokensAndRevocation) {
    Surface s;
    const ApiResult sign_up = s.call("sign_up_password",
                                     {{"email", "ada@example.com"},
                                      {"password", "correct horse battery"},
                                      {"identity_document", "{}"}});
    ASSERT_TRUE(sign_up.ok) << sign_up.error.message;
    const std::string browser_secret = sign_up.value["secret"];
    const std::string user_id = sign_up.value["session"]["user_id"];

    auto principal = s.store.context_for(browser_secret);
    ASSERT_TRUE(principal) << principal.error().message;
    EXPECT_EQ(principal->user_id, user_id);

    const ApiResult provider = s.call("register_oauth_provider",
                                      {{"provider", "stub"},
                                       {"issuer", "https://stub.example.test"},
                                       {"client_id", "lake-test"},
                                       {"client_secret", "test-client-secret"},
                                       {"scopes", json::array({"openid", "email"})}});
    ASSERT_TRUE(provider.ok) << provider.error.message;
    ApiContext user_context;
    user_context.store = &s.store;
    user_context.authorized = true;
    user_context.acting_user_id = principal->user_id;
    user_context.acting_identity = principal->taap_id;
    user_context.instance_kek = s.ctx.instance_kek;
    user_context.session_credential = true;
    user_context.scopes = principal->scopes;

    const ApiResult enrollment = s.registry.call("enroll_mfa", json::object(), user_context);
    ASSERT_TRUE(enrollment.ok) << enrollment.error.message;
    EXPECT_TRUE(
        enrollment.value["otpauth_uri"].get<std::string>().starts_with("otpauth://totp/lake:"));
    ASSERT_EQ(enrollment.value["recovery_codes"].size(), 8);
    EXPECT_FALSE(enrollment.value.contains("secret"));
    const std::string first_recovery = enrollment.value["recovery_codes"][0];
    const std::string second_recovery = enrollment.value["recovery_codes"][1];

    const json sign_in_input = {{"email", "ada@example.com"},
                                {"password", "correct horse battery"}};
    const ApiResult mfa_required = s.call("sign_in_password", sign_in_input);
    EXPECT_EQ(mfa_required.error.code, "mfa_required");
    const ApiResult verified = s.call("verify_mfa",
                                      {{"email", "ada@example.com"},
                                       {"password", "correct horse battery"},
                                       {"code", first_recovery}});
    ASSERT_TRUE(verified.ok) << verified.error.message;
    EXPECT_TRUE(s.store.context_for(verified.value["secret"]));
    const ApiResult reused = s.call("verify_mfa",
                                    {{"email", "ada@example.com"},
                                     {"password", "correct horse battery"},
                                     {"code", first_recovery}});
    EXPECT_FALSE(reused.ok);

    const ApiResult stale_disable =
        s.registry.call("disable_mfa", {{"code", first_recovery}}, user_context);
    EXPECT_EQ(stale_disable.error.code, "unauthorized");
    const ApiResult disabled =
        s.registry.call("disable_mfa", {{"code", second_recovery}}, user_context);
    ASSERT_TRUE(disabled.ok) << disabled.error.message;
    EXPECT_TRUE(s.call("sign_in_password", sign_in_input).ok);

    const ApiResult token =
        s.registry.call("create_token",
                        {{"name", "read sessions"}, {"scopes", json::array({"list_sessions"})}},
                        user_context);
    ASSERT_TRUE(token.ok) << token.error.message;
    EXPECT_FALSE(token.value["session"]["expires_at"].get<std::string>().empty());

    auto token_principal = s.store.context_for(token.value["secret"]);
    ASSERT_TRUE(token_principal);
    ApiContext token_context;
    token_context.store = &s.store;
    token_context.authorized = true;
    token_context.acting_user_id = token_principal->user_id;
    token_context.acting_identity = token_principal->taap_id;
    token_context.instance_kek = s.ctx.instance_kek;
    token_context.session_credential = true;
    token_context.scopes = token_principal->scopes;
    EXPECT_TRUE(s.registry.call("list_sessions", json::object(), token_context).ok);
    EXPECT_EQ(
        s.registry.call("create_token", {{"name", "no"}, {"scopes", json::array()}}, token_context)
            .error.code,
        "unauthorized");

    const std::string session_id = sign_up.value["session"]["id"];
    ASSERT_TRUE(s.store.revoke_session(user_id, session_id));
    EXPECT_FALSE(s.store.context_for(browser_secret));
    const ApiResult static_without_reason = s.registry.call(
        "create_token",
        {{"name", "legacy"}, {"scopes", json::array({"list_sessions"})}, {"static", true}},
        user_context);
    EXPECT_EQ(static_without_reason.error.code, "invalid_input");
    const ApiResult static_token = s.registry.call("create_token",
                                                   {{"name", "legacy"},
                                                    {"scopes", json::array({"list_sessions"})},
                                                    {"static", true},
                                                    {"reason", "migration"}},
                                                   user_context);
    ASSERT_TRUE(static_token.ok) << static_token.error.message;
    EXPECT_TRUE(static_token.value["session"]["expires_at"].is_null());
    auto audit_events = s.store.list_audit_events(lake::audit::Filter{.limit = 1000});
    ASSERT_TRUE(audit_events);
    EXPECT_TRUE(std::ranges::any_of(*audit_events, [](const lake::audit::Event& event) {
        return event.draft.action == "create_token" && event.draft.outcome == "success";
    }));
}
