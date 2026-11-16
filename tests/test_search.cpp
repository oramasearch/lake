// SPDX-License-Identifier: MIT

#include "audit_test_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <liquid/liquid.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/search/index.hpp"
#include "temporary_directory.hpp"

namespace {

class TemporaryStore {
public:
    TemporaryStore() {
        static std::atomic<unsigned long> next{0};
        path_ = lake::test::fixture_path("search-" + std::to_string(++next));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryStore() {
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

std::string create_change(const std::filesystem::path& path,
                          const std::vector<std::pair<std::string, std::string>>& files) {
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
    for (const auto& [file, contents] : files) {
        const auto written = liquid::runtime::block_on(repository.write(file, bytes(contents)));
        EXPECT_TRUE(written) << written.error().message;
    }
    const auto change = liquid::runtime::block_on(repository.cut("search fixture"));
    EXPECT_TRUE(change) << change.error().message;
    return change ? liquid::to_string(change->value) : "";
}

std::string update_change(const std::filesystem::path& path,
                          const std::vector<std::pair<std::string, std::string>>& files) {
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(path.parent_path().parent_path() / "store"));
    EXPECT_TRUE(customer_store) << customer_store.error().message;
    if (!customer_store) {
        return {};
    }
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::open(path, options));
    EXPECT_TRUE(opened) << opened.error().message;
    if (!opened) {
        return {};
    }
    auto repository = std::move(*opened);
    const auto removed_old_a = liquid::runtime::block_on(repository.remove("old-a.txt"));
    if (!removed_old_a && removed_old_a.error().code != liquid::errc::not_found) {
        ADD_FAILURE() << removed_old_a.error().message;
    }
    const auto removed_old_b = liquid::runtime::block_on(repository.remove("old-b.txt"));
    if (!removed_old_b && removed_old_b.error().code != liquid::errc::not_found) {
        ADD_FAILURE() << removed_old_b.error().message;
    }
    for (const auto& [file, contents] : files) {
        const auto written = liquid::runtime::block_on(repository.write(file, bytes(contents)));
        EXPECT_TRUE(written) << written.error().message;
    }
    const auto change = liquid::runtime::block_on(repository.cut("updated search fixture"));
    EXPECT_TRUE(change) << change.error().message;
    return change ? liquid::to_string(change->value) : "";
}

lake::search::Publish
publication(const std::string& customer, const std::string& repository, const std::string& change) {
    return {customer, repository, "main", change};
}

TEST(Search, ArtifactMembersUseRegistryForDebianAndOciLayers) {
    TemporaryStore store;
    const auto fixture_directory = store.path() / "fixtures";
    std::filesystem::create_directories(fixture_directory);
    const auto script = fixture_directory / "build.py";
    std::ofstream output(script);
    output << R"PY(import gzip, io, pathlib, tarfile, sys
p = pathlib.Path(sys.argv[1])
def tar(name, text):
    data = text.encode()
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w') as archive:
        entry = tarfile.TarInfo(name)
        entry.size = len(data)
        archive.addfile(entry, io.BytesIO(data))
    return stream.getvalue()
def ar(entries):
    data = bytearray(b'!<arch>\n')
    for name, body in entries:
        header = (name + '/').ljust(16) + '0'.ljust(12) + '0'.ljust(6) + '0'.ljust(6) + '100644'.ljust(8) + str(len(body)).ljust(10) + '`\n'
        data.extend(header.encode())
        data.extend(body)
        if len(body) % 2: data.extend(b'\n')
    return data
gzip_bytes = lambda value: gzip.compress(value, mtime=0)
(p / 'package.deb').write_bytes(ar([('debian-binary', b'2.0\n'), ('control.tar.gz', gzip_bytes(tar('control', 'Package: fixture\n'))), ('data.tar.gz', gzip_bytes(tar('usr/src/debian.cpp', 'void deb_artifact_symbol() {}\n')))]))
(p / 'layer.tar.gz').write_bytes(gzip_bytes(tar('app/layer.cpp', 'void oci_layer_symbol() {}\n')))
)PY";
    output.close();
    ASSERT_EQ(
        std::system(
            ("python3 \"" + script.string() + "\" \"" + fixture_directory.string() + "\"").c_str()),
        0);
    const auto repository_path = store.path() / "customer" / "repositories" / "artifacts";
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(store.path() / "customer" / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::create(repository_path, options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    std::map<std::string, liquid::digest> output_blobs;
    for (const std::string& name : {"package.deb", "layer.tar.gz"}) {
        std::ifstream input(fixture_directory / name, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>{input}, {}};
        ASSERT_TRUE(liquid::runtime::block_on(repository.write(name, bytes(contents))));
        const auto content = liquid::hash(liquid::algorithm::blake3, bytes(contents));
        ASSERT_TRUE(content) << content.error().message;
        const auto blob = liquid::runtime::block_on(repository.store_remote_blob(
            {.chunks = {*content},
             .size = contents.size(),
             .chunk_sizes = {contents.size()},
             .content = *content,
             .remote = liquid::remote_blob_location{"fixture", "artifact"}}));
        ASSERT_TRUE(blob) << blob.error().message;
        output_blobs.emplace(name, *blob);
    }
    const auto root = liquid::runtime::block_on(repository.root_digest());
    const auto tree = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_TRUE(tree) << tree.error().message;
    liquid::action_result result;
    for (const std::string& name : {"package.deb", "layer.tar.gz"}) {
        result.outputs.push_back({name, 0100644, output_blobs.at(name)});
    }
    result.stdout_output = result.outputs.front().blob;
    result.stderr_output = result.outputs.front().blob;
    const auto recorded = liquid::runtime::block_on(
        liquid::build::record_remote_result(repository,
                                            {.command = {"fixture"},
                                             .outputs = {"package.deb", "layer.tar.gz"},
                                             .inputs = *root,
                                             .toolchain = *root,
                                             .platform = {"macos", "arm64"}},
                                            result));
    ASSERT_TRUE(recorded) << recorded.error().message;
    const std::string version = liquid::to_string(recorded->value);
    lake::search::Service service(store.path());
    ASSERT_TRUE(service.reindex_artifacts("customer", "artifacts"));
    lake::control::InMemoryStore control;
    const auto deb = service.query(
        {"deb_artifact_symbol in:artifact:package.deb@" + version, "customer", "artifacts", "", 20},
        "admin",
        control);
    ASSERT_TRUE(deb) << deb.error().message;
    ASSERT_FALSE(deb->items.empty());
    EXPECT_EQ(deb->items.front().line, 1U);
    const auto oci = service.query(
        {"oci_layer_symbol in:artifact:layer.tar.gz@" + version, "customer", "artifacts", "", 20},
        "admin",
        control);
    ASSERT_TRUE(oci) << oci.error().message;
    ASSERT_EQ(oci->items.size(), 1U);
    EXPECT_NE(oci->items.front().path.find("app/layer.cpp"), std::string::npos);
}

} // namespace

TEST(Search, IndexesSharedBlobOnceAndReloadsTheManifest) {
    TemporaryStore store;
    const auto root = store.path();
    const std::string first = create_change(root / "customer" / "repositories" / "first",
                                            {{"one.txt", "shared orbit token"}});
    const std::string second = create_change(root / "customer" / "repositories" / "second",
                                             {{"two.txt", "shared orbit token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "first", first)));
    ASSERT_TRUE(service.publish(publication("customer", "second", second)));

    const auto hits = service.query({"orbit", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(hits) << hits.error().message;
    ASSERT_EQ(hits->items.size(), 2U);
    EXPECT_EQ(hits->mode, lake::search::QueryMode::Hot);

    std::ifstream input(root / "customer" / "index" / "blobs.json");
    const nlohmann::json blobs = nlohmann::json::parse(input);
    ASSERT_EQ(blobs.size(), 1U);

    lake::search::Service reopened(root);
    const auto reloaded = reopened.query({"orbit", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(reloaded) << reloaded.error().message;
    EXPECT_EQ(reloaded->items.size(), 2U);

    const std::filesystem::path manifest_path = root / "customer" / "index" / "manifest.json";
    std::ifstream manifest_input(manifest_path);
    nlohmann::json legacy_manifest = nlohmann::json::parse(manifest_input);
    legacy_manifest["version"] = 1;
    for (nlohmann::json& active : legacy_manifest["active"]) {
        active.erase("kind");
        const std::filesystem::path segment_path =
            manifest_path.parent_path() / active.at("segment").get<std::string>();
        std::ifstream segment_input(segment_path);
        nlohmann::json legacy_segment = nlohmann::json::parse(segment_input);
        legacy_segment["version"] = 1;
        legacy_segment.erase("kind");
        std::ofstream segment_output(segment_path);
        segment_output << legacy_segment.dump(2);
    }
    std::ofstream manifest_output(manifest_path);
    manifest_output << legacy_manifest.dump(2);
    manifest_output.close();

    lake::search::Service legacy(root);
    const auto legacy_result = legacy.query({"orbit", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(legacy_result) << legacy_result.error().message;
    EXPECT_EQ(legacy_result->items.size(), 2U);
}

// A deleted workspace's repositories leave the index (lake#514): their segments and the blobs only
// they used go, a blob another repository shares stays, and a repeat is harmless.
TEST(Search, DroppingARepositoryRemovesItsSegmentsAndUnsharedBlobs) {
    TemporaryStore store;
    const auto root = store.path();
    const std::string kept = create_change(root / "customer" / "repositories" / "kept",
                                           {{"one.txt", "shared orbit token"}});
    const std::string doomed = create_change(
        root / "customer" / "repositories" / "doomed",
        {{"two.txt", "shared orbit token"}, {"three.txt", "only comet token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "kept", kept)));
    ASSERT_TRUE(service.publish(publication("customer", "doomed", doomed)));
    ASSERT_EQ(service.query({"comet", "customer", "", "", 20}, "admin", control)->items.size(), 1U);

    ASSERT_TRUE(service.drop_repository("customer", "doomed"));
    EXPECT_TRUE(service.query({"comet", "customer", "", "", 20}, "admin", control)->items.empty());
    const auto orbit = service.query({"orbit", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(orbit) << orbit.error().message;
    ASSERT_EQ(orbit->items.size(), 1U);
    std::ifstream input(root / "customer" / "index" / "blobs.json");
    EXPECT_EQ(nlohmann::json::parse(input).size(), 1U);
    std::ifstream manifest(root / "customer" / "index" / "manifest.json");
    EXPECT_EQ(nlohmann::json::parse(manifest)["active"].size(), 1U);
    EXPECT_TRUE(service.drop_repository("customer", "doomed"));
    EXPECT_TRUE(service.drop_repository("customer", "never-indexed"));

    lake::search::Service reopened(root);
    EXPECT_TRUE(reopened.query({"comet", "customer", "", "", 20}, "admin", control)->items.empty());
    EXPECT_EQ(reopened.query({"orbit", "customer", "", "", 20}, "admin", control)->items.size(), 1U);
}

TEST(Search, PublicationMakesEveryQueryObserveOneCompleteChange) {
    TemporaryStore store;
    const auto root = store.path();
    const auto repository = root / "customer" / "repositories" / "repo";
    const std::string old_change = create_change(
        repository, {{"old-a.txt", "atomictoken old a"}, {"old-b.txt", "atomictoken old b"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "repo", old_change)));
    const std::string new_change = update_change(
        repository, {{"new-a.txt", "atomictoken new a"}, {"new-b.txt", "atomictoken new b"}});

    std::thread publisher([&] {
        const auto published = service.publish(publication("customer", "repo", new_change));
        EXPECT_TRUE(published) << published.error().message;
    });
    for (int index = 0; index < 100; ++index) {
        const auto hits =
            service.query({"atomictoken", "customer", "repo", "main", 20}, "admin", control);
        ASSERT_TRUE(hits) << hits.error().message;
        ASSERT_EQ(hits->items.size(), 2U);
        const bool old_paths = hits->items.at(0).path.starts_with("old-") &&
                               hits->items.at(1).path.starts_with("old-");
        const bool new_paths = hits->items.at(0).path.starts_with("new-") &&
                               hits->items.at(1).path.starts_with("new-");
        EXPECT_TRUE(old_paths || new_paths);
    }
    publisher.join();
}

TEST(Search, TagSnapshotIsImmutableAndReturnsOnlyItsTaggedChange) {
    TemporaryStore store;
    const auto root = store.path();
    const auto repository = root / "customer" / "repositories" / "repo";
    const std::string tagged_change =
        create_change(repository, {{"old-a.txt", "tagged historical token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(
        {"customer", "repo", "v1.0.0", tagged_change, lake::search::PublicationKind::TagSnapshot}));

    const std::string branch_change =
        update_change(repository, {{"current.txt", "current branch token"}});
    ASSERT_TRUE(service.publish(publication("customer", "repo", branch_change)));
    ASSERT_TRUE(service.publish({"customer", "repo", "v1.0.0", branch_change}));

    const auto tag = service.query({"tagged", "customer", "repo", "v1.0.0", 20}, "admin", control);
    ASSERT_TRUE(tag) << tag.error().message;
    EXPECT_EQ(tag->mode, lake::search::QueryMode::Snapshot);
    ASSERT_EQ(tag->items.size(), 1U);
    EXPECT_EQ(tag->items.front().change, tagged_change);

    const auto current_at_tag =
        service.query({"current", "customer", "repo", "v1.0.0", 20}, "admin", control);
    ASSERT_TRUE(current_at_tag) << current_at_tag.error().message;
    EXPECT_EQ(current_at_tag->mode, lake::search::QueryMode::Snapshot);
    EXPECT_TRUE(current_at_tag->items.empty());

    const auto moved = service.publish(
        {"customer", "repo", "v1.0.0", branch_change, lake::search::PublicationKind::TagSnapshot});
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error().code, "conflict");

    lake::search::Service reopened(root);
    const auto reloaded =
        reopened.query({"tagged", "customer", "repo", "v1.0.0", 20}, "admin", control);
    ASSERT_TRUE(reloaded) << reloaded.error().message;
    EXPECT_EQ(reloaded->mode, lake::search::QueryMode::Snapshot);
    ASSERT_EQ(reloaded->items.size(), 1U);
    EXPECT_EQ(reloaded->items.front().change, tagged_change);
}

TEST(Search, HistoryScanFindsTheOneMatchingChangeAmongOneThousand) {
    TemporaryStore store;
    const auto root = store.path();
    const auto repository_path = root / "customer" / "repositories" / "history";
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root / "customer" / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::create(repository_path, options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("history.txt", bytes("ordinary"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("history change 0")));

    std::string matching_change;
    for (int index = 1; index < 1'000; ++index) {
        if (index == 500) {
            ASSERT_TRUE(liquid::runtime::block_on(
                repository.write("history.txt", bytes("one-thousand-change-needle"))));
        } else if (index == 501) {
            ASSERT_TRUE(
                liquid::runtime::block_on(repository.write("history.txt", bytes("ordinary"))));
        }
        const auto change =
            liquid::runtime::block_on(repository.cut("history change " + std::to_string(index)));
        ASSERT_TRUE(change) << change.error().message;
        if (index == 500) {
            matching_change = liquid::to_string(change->value);
        }
    }

    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    const lake::search::Query query{.text = "one-thousand-change-needle",
                                    .customer = "customer",
                                    .repository = "history",
                                    .limit = 20,
                                    .history = true};
    const auto result = service.query(query, "admin", control);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->mode, lake::search::QueryMode::Scan);
    EXPECT_EQ(result->scanned_changes, 1'000U);
    ASSERT_EQ(result->items.size(), 1U);
    EXPECT_EQ(result->items.front().change, matching_change);

    const lake::search::Query exact_query{.text = "one-thousand-change-needle",
                                          .customer = "customer",
                                          .repository = "history",
                                          .limit = 20,
                                          .change = matching_change};
    const auto exact = service.query(exact_query, "admin", control);
    ASSERT_TRUE(exact) << exact.error().message;
    EXPECT_EQ(exact->mode, lake::search::QueryMode::Scan);
    EXPECT_EQ(exact->scanned_changes, 1U);
    ASSERT_EQ(exact->items.size(), 1U);
    EXPECT_EQ(exact->items.front().change, matching_change);

    std::ifstream input(root / "customer" / "index" / "blobs.json");
    const nlohmann::json blobs = nlohmann::json::parse(input);
    EXPECT_EQ(blobs.size(), 2U);
}

TEST(Search, BatchAuthorizationFiltersRepositoryCandidates) {
    TemporaryStore store;
    const auto root = store.path();
    const std::string allowed = create_change(root / "customer" / "repositories" / "allowed",
                                              {{"visible.txt", "access token"}});
    const std::string denied = create_change(root / "customer" / "repositories" / "denied",
                                             {{"hidden.txt", "access token"}});
    lake::control::InMemoryStore control;
    const auto customer =
        control.create_policy_object({"customer", "customer", "", "admin", "customer", true});
    ASSERT_TRUE(customer);
    const auto allowed_object = control.create_policy_object(
        {"repository", "allowed", customer->id, "admin", "allowed", true});
    ASSERT_TRUE(allowed_object);
    ASSERT_TRUE(control.create_policy_object(
        {"repository", "denied", customer->id, "admin", "denied", true}));
    ASSERT_TRUE(control.create_grant(
        {"reader", allowed_object->id, "read", "sha512-256:read", "admin", "grant"}));
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "allowed", allowed)));
    ASSERT_TRUE(service.publish(publication("customer", "denied", denied)));

    const auto hits = service.query({"access", "customer", "", "", 20}, "reader", control);
    ASSERT_TRUE(hits) << hits.error().message;
    ASSERT_EQ(hits->items.size(), 1U);
    EXPECT_EQ(hits->items.front().repository, "allowed");
}

TEST(Search, SubstringRegexAndRankedQueriesUseTheirExpectedOrder) {
    TemporaryStore store;
    const auto root = store.path();
    const std::string change = create_change(root / "customer" / "repositories" / "ranked",
                                             {{"first.txt", "quick brown fox alpha alpha alpha"},
                                              {"second.txt", "quick brown fox alpha"},
                                              {"third.txt", "unrelated"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "ranked", change)));

    const auto ranked = service.query({"alpha", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(ranked) << ranked.error().message;
    ASSERT_EQ(ranked->items.size(), 2U);
    EXPECT_EQ(ranked->items.front().path, "first.txt");

    const auto substring =
        service.query({"\"quick brown\"", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(substring) << substring.error().message;
    EXPECT_EQ(substring->items.size(), 2U);

    const auto regex =
        service.query({"/quick\\s+brown/", "customer", "", "", 20}, "admin", control);
    ASSERT_TRUE(regex) << regex.error().message;
    EXPECT_EQ(regex->items.size(), 2U);

    // lake#984: a pattern that can backtrack without end, or an overlong one, is refused.
    for (const std::string pattern : {"/(a+)+$/", "/(x*y?)*/", "/(\\w+\\s?)+z/"}) {
        const auto refused = service.query({pattern, "customer", "", "", 20}, "admin", control);
        ASSERT_FALSE(refused) << pattern;
        EXPECT_EQ(refused.error().code, "invalid_input") << pattern;
    }
    const auto overlong =
        service.query({"/" + std::string(300, 'a') + "/", "customer", "", "", 20}, "admin", control);
    ASSERT_FALSE(overlong);
    EXPECT_EQ(overlong.error().code, "invalid_input");
}

TEST(Search, RegisteredCapabilityReturnsAuthorizedHits) {
    TemporaryStore store;
    const auto root = store.path();
    const std::string change = create_change(root / "customer" / "repositories" / "repository",
                                             {{"hit.txt", "registry search fixture"}});
    lake::search::Service service(root);
    ASSERT_TRUE(service.publish(publication("customer", "repository", change)));

    lake::api::Registry registry;
    lake::api::register_core_capabilities(registry, "test");
    lake::api::ApiContext context;
    lake::control::InMemoryStore control;
    context.store = &control;
    context.search = &service;
    context.authorized = true;
    context.acting_identity = "admin";
    const auto result =
        registry.call("search", {{"query", "registry"}, {"customer", "customer"}}, context);

    ASSERT_TRUE(result.ok) << result.error.message;
    ASSERT_EQ(result.value.at("items").size(), 1U);
    EXPECT_EQ(result.value.at("mode"), "hot");
    EXPECT_EQ(result.value.at("scanned_changes"), 0U);
    EXPECT_EQ(result.value.at("items").front().at("path"), "hit.txt");
    const auto events = lake::test::audit_events(control, {.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_FALSE(events->empty());
    EXPECT_EQ(events->front().draft.details.value("mode", ""), "hot");
}

// ---- workspace indexes (lake#381) ------------------------------------------------------------

// A workspace's index objects held in memory, counting what is read and written.
class MemoryObjects final : public lake::search::IndexObjects {
public:
    lake::search::Result<std::optional<std::string>> get(const std::string& name) override {
        std::lock_guard lock(mutex_);
        ++gets;
        const auto found = objects.find(name);
        if (found == objects.end()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{found->second};
    }
    lake::search::Result<void> put(const std::string& name, std::string bytes, bool once) override {
        std::lock_guard lock(mutex_);
        if (fail_puts > 0) {
            --fail_puts;
            return std::unexpected(lake::search::Error{"unavailable", "the workspace Zetta did not answer"});
        }
        if (once && objects.contains(name)) {
            return std::unexpected(lake::search::Error{"conflict", "exists"});
        }
        ++puts;
        objects[name] = std::move(bytes);
        return {};
    }
    lake::search::Result<void> remove(const std::string& name) override {
        std::lock_guard lock(mutex_);
        objects.erase(name);
        return {};
    }
    lake::search::Result<std::vector<std::string>> list() override {
        std::lock_guard lock(mutex_);
        std::vector<std::string> output;
        for (const auto& [name, bytes] : objects) {
            output.push_back(name);
        }
        return output;
    }
    lake::search::Result<void> sync() override {
        return {};
    }

    std::map<std::string, std::string> snapshot() {
        std::lock_guard lock(mutex_);
        return objects;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        objects.clear();
    }
    void erase_prefix(const std::string& prefix) {
        std::lock_guard lock(mutex_);
        std::erase_if(objects, [&](const auto& item) { return item.first.starts_with(prefix); });
    }

    std::map<std::string, std::string> objects;
    int gets = 0;
    int puts = 0;
    int fail_puts = 0;

private:
    std::mutex mutex_;
};

// Switched workspaces with one repository each, lent to search the way lake's repository store
// lends them: under one lane per workspace, with the workspace's index objects. A rebuild is
// queued and run by drain(), as lake's maintenance thread runs it.
struct Workspaces {
    TemporaryStore directory;
    std::filesystem::path root = directory.path();
    std::map<std::string, std::string> workspace_of;
    std::map<std::string, std::string> space_of{{"ws-one", "space"}, {"ws-two", "space"}};
    std::map<std::string, std::shared_ptr<MemoryObjects>> objects{
        {"ws-one", std::make_shared<MemoryObjects>()}, {"ws-two", std::make_shared<MemoryObjects>()}};
    std::map<std::string, std::unique_ptr<std::mutex>> lanes;
    std::unique_ptr<liquid::store> store;
    std::map<std::string, std::unique_ptr<liquid::repository>> repositories;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> releases;
    std::map<std::string, int> lends;
    // A workspace's lane full of requests: admission is refused, lake's own work still waits.
    std::set<std::string> busy;
    // Borrows of a repository refused this many more times, and lends of a workspace.
    std::map<std::string, int> refuse_borrow;
    std::map<std::string, int> fail_lend;
    // Called by the index's write budget, which a rebuild reads just before it writes.
    std::function<void()> before_commit;
    std::vector<std::pair<std::string, std::string>> queued;
    std::mutex queue_mutex;

    Workspaces() {
        lanes["ws-one"] = std::make_unique<std::mutex>();
        lanes["ws-two"] = std::make_unique<std::mutex>();
    }

    // A repository in `workspace` whose `main` names one change with `files`; answers the change.
    std::string add(const std::string& repository, const std::string& workspace,
                    const std::vector<std::pair<std::string, std::string>>& files) {
        workspace_of[repository] = workspace;
        if (!store) {
            auto opened = liquid::runtime::block_on(liquid::store::open(root / "space" / "store"));
            EXPECT_TRUE(opened) << opened.error().message;
            store = std::make_unique<liquid::store>(std::move(*opened));
        }
        liquid::repository_options options;
        options.store = store.get();
        auto created = liquid::runtime::block_on(
            liquid::repository::create(root / "space" / "repositories" / repository, options));
        EXPECT_TRUE(created) << created.error().message;
        for (const auto& [file, contents] : files) {
            EXPECT_TRUE(liquid::runtime::block_on(created->write(file, bytes(contents))));
        }
        const auto change = liquid::runtime::block_on(created->cut("workspace fixture"));
        EXPECT_TRUE(change) << change.error().message;
        EXPECT_TRUE(liquid::runtime::block_on(created->set_ref("main", *change)));
        repositories[repository] = std::make_unique<liquid::repository>(std::move(*created));
        return liquid::to_string(change->value);
    }

    // A later change on `main` with one more file.
    std::string advance(const std::string& repository, const std::string& file, const std::string& text) {
        auto& opened = *repositories.at(repository);
        EXPECT_TRUE(liquid::runtime::block_on(opened.write(file, bytes(text))));
        const auto change = liquid::runtime::block_on(opened.cut("later"));
        EXPECT_TRUE(change);
        EXPECT_TRUE(liquid::runtime::block_on(opened.set_ref("main", *change)));
        return liquid::to_string(change->value);
    }

    lake::search::WorkspaceIndex index(const std::string& workspace) {
        lake::search::WorkspaceIndex output;
        output.customer = space_of.at(workspace);
        output.workspace = workspace;
        output.generation = 1;
        output.objects = objects.at(workspace);
        output.repositories = [this, workspace]() -> lake::search::Result<std::vector<std::string>> {
            std::vector<std::string> ids;
            for (const auto& [repository, owner] : workspace_of) {
                if (owner == workspace) {
                    ids.push_back(repository);
                }
            }
            return ids;
        };
        output.tags = [this](const std::string& repository)
            -> lake::search::Result<std::vector<std::pair<std::string, std::string>>> {
            return releases[repository];
        };
        output.write_budget = [this]() -> std::optional<std::uint64_t> {
            if (before_commit) {
                auto hook = std::move(before_commit);
                before_commit = nullptr;
                hook();
            }
            return std::nullopt;
        };
        return output;
    }

    lake::search::Result<std::shared_ptr<void>> hold(const std::string& workspace, bool lake_work) {
        if (!lake_work && busy.contains(workspace)) {
            return std::unexpected(lake::search::Error{"store_unavailable", "the store is busy"});
        }
        ++lends[workspace];
        return std::shared_ptr<void>(std::make_shared<std::unique_lock<std::mutex>>(*lanes.at(workspace)));
    }

    void configure(lake::search::Service& service) {
        service.set_repository_access(
            [this](const std::string&, const std::string& repository, bool lake_work)
                -> lake::search::Result<lake::search::BorrowedRepository> {
                if (!workspace_of.contains(repository)) {
                    return std::unexpected(lake::search::Error{"not_found", "no repository"});
                }
                if (refuse_borrow[repository] > 0) {
                    --refuse_borrow[repository];
                    return std::unexpected(lake::search::Error{"store_unavailable", "refused"});
                }
                const std::string workspace = workspace_of.at(repository);
                auto held = hold(workspace, lake_work);
                if (!held) {
                    return std::unexpected(held.error());
                }
                return lake::search::BorrowedRepository{*held, repositories.at(repository).get(),
                                                        index(workspace)};
            });
        lake::search::WorkspaceAccess access;
        access.lend = [this](const std::string& customer, const std::string& workspace, bool lake_work)
            -> lake::search::Result<lake::search::BorrowedWorkspace> {
            if (!objects.contains(workspace) || (!customer.empty() && space_of.at(workspace) != customer)) {
                return std::unexpected(lake::search::Error{"invalid_input", "no such workspace"});
            }
            if (fail_lend[workspace] > 0) {
                --fail_lend[workspace];
                return std::unexpected(lake::search::Error{"store_unavailable", "the workspace Zetta is away"});
            }
            auto held = hold(workspace, lake_work);
            if (!held) {
                return std::unexpected(held.error());
            }
            return lake::search::BorrowedWorkspace{*held, index(workspace)};
        };
        access.lend_if_open = [this](const std::string& workspace)
            -> lake::search::Result<std::optional<lake::search::BorrowedWorkspace>> {
            auto held = hold(workspace, true);
            return std::optional<lake::search::BorrowedWorkspace>{
                lake::search::BorrowedWorkspace{*held, index(workspace)}};
        };
        access.switched = [this](const std::string& customer)
            -> lake::search::Result<std::vector<std::pair<std::string, std::string>>> {
            std::vector<std::pair<std::string, std::string>> output;
            for (const auto& [workspace, space] : space_of) {
                if (customer.empty() || customer == space) {
                    output.emplace_back(space, workspace);
                }
            }
            return output;
        };
        access.route = [this](const std::string&, const std::string& repository)
            -> lake::search::Result<std::optional<std::string>> {
            return std::optional<std::string>{workspace_of.at(repository)};
        };
        access.schedule = [this](const std::string& customer, const std::string& workspace,
                                 std::chrono::milliseconds) {
            std::lock_guard lock(queue_mutex);
            queued.emplace_back(customer, workspace);
        };
        service.set_workspace_access(std::move(access));
    }

    // Runs the queued jobs, as lake's search index worker does (a retry's delay is not waited).
    void drain(lake::search::Service& service, bool expect_success = true) {
        std::vector<std::pair<std::string, std::string>> jobs;
        {
            std::lock_guard lock(queue_mutex);
            jobs.swap(queued);
        }
        for (const auto& [customer, workspace] : jobs) {
            const auto rebuilt = service.rebuild_workspace(customer, workspace);
            if (expect_success) {
                EXPECT_TRUE(rebuilt) << rebuilt.error().message;
            }
        }
    }

    // The first search of a workspace answers "building" and queues its rebuild; after it, the
    // search answers.
    lake::search::QueryResult ready(lake::search::Service& service, const lake::search::Query& query,
                                    lake::control::Store& control) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto result = service.query(query, "admin", control);
            EXPECT_TRUE(result) << result.error().message;
            if (!result || result->building_workspaces.empty()) {
                return result ? *result : lake::search::QueryResult{};
            }
            drain(service);
        }
        ADD_FAILURE() << "the index never became ready";
        return {};
    }

    static lake::search::Query in(const std::string& text, const std::string& workspace,
                                  const std::string& customer = "space") {
        return lake::search::Query{text, customer, "", "", 20, "", false, workspace};
    }
};

std::set<std::string> hit_workspaces(const lake::search::QueryResult& result) {
    std::set<std::string> output;
    for (const auto& hit : result.items) {
        output.insert(hit.workspace);
    }
    return output;
}

std::size_t names_with(const std::map<std::string, std::string>& objects, const std::string& prefix) {
    return static_cast<std::size_t>(std::ranges::count_if(
        objects, [&](const auto& item) { return item.first.starts_with(prefix); }));
}

TEST(SearchWorkspaces, TwoWorkspacesKeepSeparateIndexesAndAReopenedServiceLoadsThem) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "alphaonly shared token"}});
    fixture.add("repo-two", "ws-two", {{"two.txt", "betaonly shared token"}});
    lake::control::InMemoryStore control;
    {
        lake::search::Service service(fixture.root);
        fixture.configure(service);
        // The first search finds the index missing: it answers "building" without waiting, and
        // the rebuild runs later, off the request.
        const auto first = service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control);
        ASSERT_TRUE(first) << first.error().message;
        EXPECT_TRUE(first->items.empty());
        EXPECT_EQ(first->building_workspaces, (std::vector<std::string>{"ws-one"}));
        EXPECT_FALSE(fixture.objects["ws-one"]->snapshot().contains("manifest.json")) << "not rebuilt inline";
        const auto one = fixture.ready(service, Workspaces::in("alphaonly", "ws-one"), control);
        ASSERT_EQ(one.items.size(), 1U);
        EXPECT_EQ(one.items[0].workspace, "ws-one");
        EXPECT_EQ(one.items[0].repository, "repo-one");
        EXPECT_TRUE(fixture.ready(service, Workspaces::in("alphaonly", "ws-two"), control).items.empty())
            << "ws-two's index holds nothing of ws-one";
        EXPECT_TRUE(service.query(Workspaces::in("betaonly", "ws-one"), "admin", control)->items.empty());
        // A space-wide search reads both, each hit naming its workspace.
        const auto both = service.query({"shared", "space", "", "", 20}, "admin", control);
        ASSERT_TRUE(both) << both.error().message;
        EXPECT_EQ(hit_workspaces(*both), (std::set<std::string>{"ws-one", "ws-two"}));
        // A search of one repository reads only its workspace's index.
        const auto scoped = service.query({"shared", "space", "repo-two", "", 20}, "admin", control);
        ASSERT_TRUE(scoped) << scoped.error().message;
        EXPECT_EQ(hit_workspaces(*scoped), (std::set<std::string>{"ws-two"}));
    }
    // Nothing of either workspace is on lake's volume, and the blobs are in runs, never one
    // catalogue.
    EXPECT_FALSE(std::filesystem::exists(fixture.root / "space" / "index"));
    for (const auto& [workspace, objects] : fixture.objects) {
        const auto held = objects->snapshot();
        EXPECT_TRUE(held.contains("manifest.json")) << workspace;
        EXPECT_FALSE(held.contains("blobs.json")) << workspace;
        EXPECT_EQ(names_with(held, "runs/"), 1U) << workspace;
        const std::string other = workspace == "ws-one" ? "betaonly" : "alphaonly";
        for (const auto& [name, bytes] : held) {
            EXPECT_EQ(bytes.find(other), std::string::npos) << workspace << " " << name;
        }
    }
    // A new process loads both from their objects, at once, and writes nothing.
    const int puts_before = fixture.objects["ws-one"]->puts + fixture.objects["ws-two"]->puts;
    lake::search::Service reopened(fixture.root);
    fixture.configure(reopened);
    const auto again = reopened.query({"shared", "space", "", "", 20}, "admin", control);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again->building_workspaces.empty());
    EXPECT_EQ(again->items.size(), 2U);
    EXPECT_EQ(fixture.objects["ws-one"]->puts + fixture.objects["ws-two"]->puts, puts_before)
        << "a reload is not a rebuild";
}

TEST(SearchWorkspaces, ASpaceWideSearchNeverLendsAWorkspaceOutsideItsSpaces) {
    Workspaces fixture;
    fixture.space_of["ws-two"] = "other";
    fixture.add("repo-one", "ws-one", {{"one.txt", "alphaonly shared token"}});
    fixture.add("repo-two", "ws-two", {{"two.txt", "betaonly shared token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    // A caller limited to "space": every space is asked for, only its own workspaces are lent.
    lake::search::Query query{"shared", "", "", "", 20, "", false, ""};
    query.spaces = std::set<std::string>{"space"};
    ASSERT_TRUE(service.query(query, "admin", control));
    fixture.drain(service);
    const auto answered = service.query(query, "admin", control);
    ASSERT_TRUE(answered) << answered.error().message;
    EXPECT_EQ(hit_workspaces(*answered), (std::set<std::string>{"ws-one"}));
    EXPECT_TRUE(answered->skipped_workspaces.empty());
    EXPECT_EQ(fixture.lends["ws-two"], 0) << "another space's workspace was lent";
    // Naming the other space's workspace, or the other space, does not reach it either.
    auto named = Workspaces::in("betaonly", "ws-two", "other");
    named.spaces = std::set<std::string>{"space"};
    const auto refused = service.query(named, "admin", control);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "invalid_input");
    lake::search::Query other{"shared", "other", "", "", 20, "", false, ""};
    other.spaces = std::set<std::string>{"space"};
    ASSERT_TRUE(service.query(other, "admin", control));
    EXPECT_EQ(fixture.lends["ws-two"], 0);
    EXPECT_FALSE(fixture.objects["ws-two"]->snapshot().contains("manifest.json")) << "nothing rebuilt";
}

TEST(SearchWorkspaces, AWipedOrPartlyLostWorkspaceIndexIsRebuiltNotServedEmpty) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "alphaonly token"}});
    fixture.add("repo-two", "ws-two", {{"two.txt", "betaonly token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_EQ(fixture.ready(service, Workspaces::in("alphaonly", "ws-one"), control).items.size(), 1U);
    ASSERT_EQ(fixture.ready(service, Workspaces::in("betaonly", "ws-two"), control).items.size(), 1U);
    // Without a restart: under a tight bound each search evicts the other workspace, so the next
    // use reads the objects again and finds what was lost.
    service.set_memory_bound(1);
    const auto cases = std::vector<std::pair<std::string, std::function<void()>>>{
        {"wiped", [&] { fixture.objects["ws-one"]->clear(); }},
        {"manifest kept, blob runs lost", [&] { fixture.objects["ws-one"]->erase_prefix("runs/"); }},
        {"manifest kept, a segment lost", [&] { fixture.objects["ws-one"]->erase_prefix("segments/"); }},
        {"manifest kept, artifact members lost", [&] { fixture.objects["ws-one"]->erase_prefix("artifacts/"); }},
    };
    for (const auto& [name, lose] : cases) {
        ASSERT_EQ(service.query(Workspaces::in("betaonly", "ws-two"), "admin", control)->items.size(), 1U);
        EXPECT_EQ(service.loaded_workspaces(), (std::vector<std::string>{"ws-two"})) << name;
        lose();
        const auto lost = service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control);
        ASSERT_TRUE(lost) << name << ": " << lost.error().message;
        EXPECT_TRUE(lost->items.empty()) << name;
        EXPECT_EQ(lost->building_workspaces, (std::vector<std::string>{"ws-one"}))
            << name << ": a partial index is never served";
        fixture.drain(service);
        EXPECT_EQ(service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control)->items.size(), 1U)
            << name;
    }
    // A rebuild on demand replaces the index and removes what it no longer names.
    fixture.objects["ws-one"]->put("segments/stale.json", "{}", false);
    service.schedule_rebuild("space", "ws-one");
    fixture.drain(service);
    EXPECT_FALSE(fixture.objects["ws-one"]->snapshot().contains("segments/stale.json"));
}

TEST(SearchWorkspaces, AFailedRebuildBacksOffInsteadOfRebuildingOnEveryRequest) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "alphaonly token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_TRUE(service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control));
    fixture.objects["ws-one"]->fail_puts = 1000;
    {
        std::vector<std::pair<std::string, std::string>> jobs;
        std::swap(jobs, fixture.queued);
        ASSERT_EQ(jobs.size(), 1U);
        EXPECT_FALSE(service.rebuild_workspace("space", "ws-one"));
    }
    for (int index = 0; index < 20; ++index) {
        const auto answered = service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control);
        ASSERT_TRUE(answered) << answered.error().message;
        EXPECT_EQ(answered->building_workspaces, (std::vector<std::string>{"ws-one"}));
    }
    EXPECT_TRUE(fixture.queued.empty()) << "a failed rebuild is not retried on every request";
    fixture.objects["ws-one"]->fail_puts = 0;
    ASSERT_TRUE(service.rebuild_workspace("space", "ws-one"));
    EXPECT_EQ(service.query(Workspaces::in("alphaonly", "ws-one"), "admin", control)->items.size(), 1U);
}

TEST(SearchWorkspaces, RebuildRepublishesReleasesAndBlobsAreWrittenOnceInRuns) {
    Workspaces fixture;
    const std::string tagged = fixture.add("repo-one", "ws-one", {{"one.txt", "tagterm original"}});
    fixture.advance("repo-one", "two.txt", "mainonly later");
    fixture.releases["repo-one"] = {{"v1", tagged}};
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_TRUE(service.query(Workspaces::in("tagterm", "ws-one"), "admin", control));
    fixture.drain(service);
    auto snapshot = Workspaces::in("tagterm", "ws-one");
    snapshot.ref = "v1";
    const auto tag = service.query(snapshot, "admin", control);
    ASSERT_TRUE(tag) << tag.error().message;
    EXPECT_EQ(tag->mode, lake::search::QueryMode::Snapshot);
    ASSERT_EQ(tag->items.size(), 1U) << "the release's snapshot came back from its record";
    EXPECT_EQ(tag->items[0].change, tagged);
    snapshot.text = "mainonly";
    EXPECT_TRUE(service.query(snapshot, "admin", control)->items.empty());

    // A push adds one file: the existing run is not rewritten, one new run holds the new text.
    const auto before = fixture.objects["ws-one"]->snapshot();
    fixture.advance("repo-one", "three.txt", "freshterm pushed");
    ASSERT_TRUE(service.reindex_artifacts("space", "repo-one"));
    const auto after = fixture.objects["ws-one"]->snapshot();
    for (const auto& [name, bytes] : before) {
        if (name.starts_with("runs/")) {
            ASSERT_TRUE(after.contains(name)) << name;
            EXPECT_EQ(after.at(name), bytes) << name << " was rewritten";
        }
    }
    EXPECT_EQ(names_with(after, "runs/"), names_with(before, "runs/") + 1);
    EXPECT_EQ(service.query(Workspaces::in("freshterm", "ws-one"), "admin", control)->items.size(), 1U);
    // A run is written once: putting it again is refused.
    const std::string run = std::ranges::find_if(after, [](const auto& item) {
                                return item.first.starts_with("runs/");
                            })->first;
    EXPECT_FALSE(fixture.objects["ws-one"]->put(run, "{}", true));
}

TEST(SearchWorkspaces, ReindexFollowsTheReferencesAndIdleWorkspacesAreEvicted) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "alphaonly token"}});
    fixture.add("repo-two", "ws-two", {{"two.txt", "betaonly token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_EQ(fixture.ready(service, Workspaces::in("alphaonly", "ws-one"), control).items.size(), 1U);
    ASSERT_EQ(fixture.ready(service, Workspaces::in("betaonly", "ws-two"), control).items.size(), 1U);
    fixture.advance("repo-one", "later.txt", "gammalater token");
    EXPECT_TRUE(service.query(Workspaces::in("gammalater", "ws-one"), "admin", control)->items.empty());
    ASSERT_TRUE(service.reindex_artifacts("space", "repo-one"));
    EXPECT_EQ(service.query(Workspaces::in("gammalater", "ws-one"), "admin", control)->items.size(), 1U);

    // Under a bound smaller than one index, only the index in use stays loaded.
    service.set_memory_bound(1);
    ASSERT_EQ(service.query(Workspaces::in("betaonly", "ws-two"), "admin", control)->items.size(), 1U);
    EXPECT_EQ(service.loaded_workspaces(), (std::vector<std::string>{"ws-two"}));
    const int gets = fixture.objects["ws-one"]->gets;
    ASSERT_EQ(service.query(Workspaces::in("gammalater", "ws-one"), "admin", control)->items.size(), 1U);
    EXPECT_GT(fixture.objects["ws-one"]->gets, gets) << "an evicted index is loaded again";
    EXPECT_EQ(service.loaded_workspaces(), (std::vector<std::string>{"ws-one"}));
    service.set_memory_bound(256ULL * 1024ULL * 1024ULL);
    ASSERT_EQ(service.query(Workspaces::in("betaonly", "ws-two"), "admin", control)->items.size(), 1U);
    EXPECT_EQ(service.loaded_workspaces().size(), 2U);
    EXPECT_GT(service.workspace_memory(), 0U);

    // A deleted repository leaves its workspace's index.
    ASSERT_TRUE(service.forget_repository("space", "ws-one", "repo-one"));
    EXPECT_TRUE(service.query(Workspaces::in("gammalater", "ws-one"), "admin", control)->items.empty());
    lake::search::Service reopened(fixture.root);
    fixture.configure(reopened);
    EXPECT_TRUE(reopened.query(Workspaces::in("alphaonly", "ws-one"), "admin", control)->items.empty());

    // Dropping a workspace forgets it and removes its objects.
    ASSERT_TRUE(service.drop_workspace("ws-one"));
    EXPECT_TRUE(fixture.objects["ws-one"]->snapshot().empty());
    EXPECT_EQ(service.loaded_workspaces(), (std::vector<std::string>{"ws-two"}));
}

TEST(SearchWorkspaces, AHistoryScanNamingAWorkspaceReadsThatWorkspaceOnly) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "scanterm token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_TRUE(service.query(Workspaces::in("scanterm", "ws-one"), "admin", control));
    fixture.drain(service);
    lake::search::Query scan{"scanterm", "space", "repo-one", "", 20, "", true, "ws-one"};
    const auto scanned = service.query(scan, "admin", control);
    ASSERT_TRUE(scanned) << scanned.error().message;
    EXPECT_EQ(scanned->mode, lake::search::QueryMode::Scan);
    EXPECT_EQ(scanned->items.size(), 1U);
    scan.workspace = "ws-two";
    const auto elsewhere = service.query(scan, "admin", control);
    ASSERT_FALSE(elsewhere);
    EXPECT_EQ(elsewhere.error().code, "invalid_input") << "not a retry: the repository is not there";
}

TEST(SearchWorkspaces, ARebuildIsLakesOwnWorkAndWaitsForABusyLane) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "busyterm token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_TRUE(service.query(Workspaces::in("busyterm", "ws-one"), "admin", control));
    // Requests fill the workspace's lane: a request is refused admission, the rebuild is not.
    fixture.busy.insert("ws-one");
    fixture.drain(service);
    EXPECT_TRUE(fixture.objects["ws-one"]->snapshot().contains("manifest.json"))
        << "the rebuild was refused admission and backed off";
    fixture.busy.clear();
    EXPECT_EQ(service.query(Workspaces::in("busyterm", "ws-one"), "admin", control)->items.size(), 1U);
}

TEST(SearchWorkspaces, WritesAndDeletionsDuringARebuildAreAppliedAfterIt) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "keptterm token"}});
    fixture.add("repo-gone", "ws-one", {{"gone.txt", "goneterm token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_TRUE(service.query(Workspaces::in("keptterm", "ws-one"), "admin", control));
    // While the rebuild is about to write: repo-one is pushed to, and repo-gone is deleted. The
    // pushed repository's first re-index borrow is refused.
    fixture.before_commit = [&] {
        fixture.advance("repo-one", "late.txt", "lateterm token");
        static_cast<void>(service.reindex_artifacts("space", "repo-one"));
        fixture.workspace_of.erase("repo-gone");
        ASSERT_TRUE(service.forget_repository("space", "ws-one", "repo-gone"));
        fixture.refuse_borrow["repo-one"] = 1;
    };
    fixture.drain(service, false);
    EXPECT_TRUE(service.query(Workspaces::in("goneterm", "ws-one"), "admin", control)->items.empty())
        << "a repository deleted during the build came back";
    // The refused re-index is kept and retried, not dropped.
    ASSERT_FALSE(fixture.queued.empty()) << "no retry was queued";
    fixture.drain(service);
    EXPECT_EQ(service.query(Workspaces::in("lateterm", "ws-one"), "admin", control)->items.size(), 1U);
    lake::search::Service reopened(fixture.root);
    fixture.configure(reopened);
    EXPECT_TRUE(reopened.query(Workspaces::in("goneterm", "ws-one"), "admin", control)->items.empty());
    EXPECT_EQ(reopened.query(Workspaces::in("lateterm", "ws-one"), "admin", control)->items.size(), 1U);
}

TEST(SearchWorkspaces, ADeletionThatCannotReachTheIndexIsRetried) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "stayterm token"}});
    fixture.add("repo-gone", "ws-one", {{"gone.txt", "goneterm token"}});
    lake::control::InMemoryStore control;
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_EQ(fixture.ready(service, Workspaces::in("goneterm", "ws-one"), control).items.size(), 1U);
    fixture.fail_lend["ws-one"] = 1;
    EXPECT_FALSE(service.forget_repository("space", "ws-one", "repo-gone"));
    ASSERT_FALSE(fixture.queued.empty()) << "the removal was only reported, not retried";
    fixture.workspace_of.erase("repo-gone");
    fixture.drain(service);
    EXPECT_TRUE(service.query(Workspaces::in("goneterm", "ws-one"), "admin", control)->items.empty());
    EXPECT_EQ(service.query(Workspaces::in("stayterm", "ws-one"), "admin", control)->items.size(), 1U);
}

TEST(SearchWorkspaces, OrphanObjectsAreSweptOnLoadAndADroppedWorkspaceKeepsNoState) {
    Workspaces fixture;
    fixture.add("repo-one", "ws-one", {{"one.txt", "orphanterm token"}});
    lake::control::InMemoryStore control;
    {
        lake::search::Service service(fixture.root);
        fixture.configure(service);
        ASSERT_EQ(fixture.ready(service, Workspaces::in("orphanterm", "ws-one"), control).items.size(), 1U);
    }
    // A commit that stopped before its manifest left objects nothing names.
    fixture.objects["ws-one"]->put("runs/orphan.json", "{}", true);
    fixture.objects["ws-one"]->put("segments/orphan.json", "{}", true);
    lake::search::Service service(fixture.root);
    fixture.configure(service);
    ASSERT_EQ(service.query(Workspaces::in("orphanterm", "ws-one"), "admin", control)->items.size(), 1U);
    EXPECT_FALSE(fixture.objects["ws-one"]->snapshot().contains("runs/orphan.json"));
    EXPECT_FALSE(fixture.objects["ws-one"]->snapshot().contains("segments/orphan.json"));
    // A deleted workspace's state goes, and a job queued for it before does not bring it back.
    service.schedule_rebuild("space", "ws-one");
    ASSERT_TRUE(service.drop_workspace("ws-one"));
    fixture.drain(service, false);
    const auto tracked = service.tracked_workspaces();
    EXPECT_TRUE(std::ranges::find(tracked, "ws-one") == tracked.end());
    EXPECT_TRUE(fixture.objects["ws-one"]->snapshot().empty()) << "a dropped workspace was rebuilt";
}

TEST(SearchBenchmark, DISABLED_BuildAndQueryTenThousandFiles) {
    TemporaryStore store;
    const auto root = store.path();
    const auto repository = root / "customer" / "repositories" / "benchmark";
    auto customer_store =
        liquid::runtime::block_on(liquid::store::open(root / "customer" / "store"));
    ASSERT_TRUE(customer_store) << customer_store.error().message;
    liquid::repository_options options;
    options.store = &*customer_store;
    auto opened = liquid::runtime::block_on(liquid::repository::create(repository, options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto source = std::move(*opened);
    for (int index = 0; index < 10'000; ++index) {
        const std::string name = "files/" + std::to_string(index) + ".txt";
        const std::string content = "benchmark token " + std::to_string(index) + " shared query";
        ASSERT_TRUE(liquid::runtime::block_on(source.write(name, bytes(content))));
    }
    const auto change = liquid::runtime::block_on(source.cut("benchmark corpus"));
    ASSERT_TRUE(change) << change.error().message;

    lake::search::Service service(root);
    lake::control::InMemoryStore control;
    const auto build_started = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        service.publish(publication("customer", "benchmark", liquid::to_string(change->value))));
    const auto build_elapsed = std::chrono::steady_clock::now() - build_started;
    const auto query_started = std::chrono::steady_clock::now();
    for (int index = 0; index < 100; ++index) {
        const auto hits = service.query({"benchmark", "customer", "", "", 20}, "admin", control);
        ASSERT_TRUE(hits) << hits.error().message;
    }
    const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
    const auto build_milliseconds =
        std::chrono::duration<double, std::milli>(build_elapsed).count();
    const auto query_microseconds =
        std::chrono::duration<double, std::micro>(query_elapsed).count() / 100.0;
    std::cout << "10k files: index build " << build_milliseconds << " ms, mean query "
              << query_microseconds << " us\n";
}
