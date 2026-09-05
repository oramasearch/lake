#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

namespace {

class TemporaryStore {
public:
    TemporaryStore() {
        static std::atomic<unsigned long> next{0};
        path_ = std::filesystem::temp_directory_path() / ("lake-search-" + std::to_string(++next));
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
    const auto events = control.list_audit_events({.limit = 100});
    ASSERT_TRUE(events) << events.error().message;
    ASSERT_FALSE(events->empty());
    EXPECT_EQ(events->front().draft.details.value("mode", ""), "hot");
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
