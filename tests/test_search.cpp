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
#include "lake/policy/authorize.hpp"
#include "lake/search/index.hpp"

namespace {

class TemporaryStore {
public:
  TemporaryStore() {
    static std::atomic<unsigned long> next{0};
    path_ = std::filesystem::temp_directory_path() /
            ("lake-search-" + std::to_string(++next));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::vector<std::byte> bytes(const std::string &value) {
  std::vector<std::byte> output;
  output.reserve(value.size());
  for (const char character : value) {
    output.push_back(static_cast<std::byte>(character));
  }
  return output;
}

std::string
create_change(const std::filesystem::path &path,
              const std::vector<std::pair<std::string, std::string>> &files) {
  auto opened = liquid::runtime::block_on(liquid::repository::create(path));
  EXPECT_TRUE(opened) << opened.error().message;
  if (!opened) {
    return {};
  }
  auto repository = std::move(*opened);
  for (const auto &[file, contents] : files) {
    const auto written =
        liquid::runtime::block_on(repository.write(file, bytes(contents)));
    EXPECT_TRUE(written) << written.error().message;
  }
  const auto change =
      liquid::runtime::block_on(repository.cut("search fixture"));
  EXPECT_TRUE(change) << change.error().message;
  return change ? liquid::to_string(change->value) : "";
}

std::string
update_change(const std::filesystem::path &path,
              const std::vector<std::pair<std::string, std::string>> &files) {
  auto opened = liquid::runtime::block_on(liquid::repository::open(path));
  EXPECT_TRUE(opened) << opened.error().message;
  if (!opened) {
    return {};
  }
  auto repository = std::move(*opened);
  const auto removed_old_a =
      liquid::runtime::block_on(repository.remove("old-a.txt"));
  if (!removed_old_a && removed_old_a.error().code != liquid::errc::not_found) {
    ADD_FAILURE() << removed_old_a.error().message;
  }
  const auto removed_old_b =
      liquid::runtime::block_on(repository.remove("old-b.txt"));
  if (!removed_old_b && removed_old_b.error().code != liquid::errc::not_found) {
    ADD_FAILURE() << removed_old_b.error().message;
  }
  for (const auto &[file, contents] : files) {
    const auto written =
        liquid::runtime::block_on(repository.write(file, bytes(contents)));
    EXPECT_TRUE(written) << written.error().message;
  }
  const auto change =
      liquid::runtime::block_on(repository.cut("updated search fixture"));
  EXPECT_TRUE(change) << change.error().message;
  return change ? liquid::to_string(change->value) : "";
}

lake::search::Publish publication(const std::string &customer,
                                  const std::string &repository,
                                  const std::string &change) {
  return {customer, repository, "main", change};
}

class OneRepositoryAuthorizer final : public lake::policy::Authorizer {
public:
  [[nodiscard]] std::vector<bool>
  authorize(const std::string &principal,
            const std::vector<lake::policy::Request> &requests) const override {
    std::vector<bool> output;
    output.reserve(requests.size());
    for (const auto &request : requests) {
      output.push_back(principal == "reader" && request.object.id == "allowed");
    }
    return output;
  }
};

} // namespace

TEST(Search, IndexesSharedBlobOnceAndReloadsTheManifest) {
  TemporaryStore store;
  const auto root = store.path();
  const std::string first =
      create_change(root / "customer" / "repositories" / "first",
                    {{"one.txt", "shared orbit token"}});
  const std::string second =
      create_change(root / "customer" / "repositories" / "second",
                    {{"two.txt", "shared orbit token"}});
  lake::search::Service service(root);
  ASSERT_TRUE(service.publish(publication("customer", "first", first)));
  ASSERT_TRUE(service.publish(publication("customer", "second", second)));

  const auto hits = service.query({"orbit", "customer", "", "", 20}, "admin");
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 2U);

  std::ifstream input(root / "customer" / "index" / "blobs.json");
  const nlohmann::json blobs = nlohmann::json::parse(input);
  ASSERT_EQ(blobs.size(), 1U);

  lake::search::Service reopened(root);
  const auto reloaded =
      reopened.query({"orbit", "customer", "", "", 20}, "admin");
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ(reloaded->size(), 2U);
}

TEST(Search, PublicationMakesEveryQueryObserveOneCompleteChange) {
  TemporaryStore store;
  const auto root = store.path();
  const auto repository = root / "customer" / "repositories" / "repo";
  const std::string old_change =
      create_change(repository, {{"old-a.txt", "atomictoken old a"},
                                 {"old-b.txt", "atomictoken old b"}});
  lake::search::Service service(root);
  ASSERT_TRUE(service.publish(publication("customer", "repo", old_change)));
  const std::string new_change =
      update_change(repository, {{"new-a.txt", "atomictoken new a"},
                                 {"new-b.txt", "atomictoken new b"}});

  std::thread publisher([&] {
    const auto published =
        service.publish(publication("customer", "repo", new_change));
    EXPECT_TRUE(published) << published.error().message;
  });
  for (int index = 0; index < 100; ++index) {
    const auto hits =
        service.query({"atomictoken", "customer", "repo", "main", 20}, "admin");
    ASSERT_TRUE(hits) << hits.error().message;
    ASSERT_EQ(hits->size(), 2U);
    const bool old_paths = hits->at(0).path.starts_with("old-") &&
                           hits->at(1).path.starts_with("old-");
    const bool new_paths = hits->at(0).path.starts_with("new-") &&
                           hits->at(1).path.starts_with("new-");
    EXPECT_TRUE(old_paths || new_paths);
  }
  publisher.join();
}

TEST(Search, BatchAuthorizationFiltersRepositoryCandidates) {
  TemporaryStore store;
  const auto root = store.path();
  const std::string allowed =
      create_change(root / "customer" / "repositories" / "allowed",
                    {{"visible.txt", "access token"}});
  const std::string denied =
      create_change(root / "customer" / "repositories" / "denied",
                    {{"hidden.txt", "access token"}});
  auto authorizer = std::make_shared<OneRepositoryAuthorizer>();
  lake::search::Service service(root, authorizer);
  ASSERT_TRUE(service.publish(publication("customer", "allowed", allowed)));
  ASSERT_TRUE(service.publish(publication("customer", "denied", denied)));

  const auto hits = service.query({"access", "customer", "", "", 20}, "reader");
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 1U);
  EXPECT_EQ(hits->front().repository, "allowed");
}

TEST(Search, SubstringRegexAndRankedQueriesUseTheirExpectedOrder) {
  TemporaryStore store;
  const auto root = store.path();
  const std::string change =
      create_change(root / "customer" / "repositories" / "ranked",
                    {{"first.txt", "quick brown fox alpha alpha alpha"},
                     {"second.txt", "quick brown fox alpha"},
                     {"third.txt", "unrelated"}});
  lake::search::Service service(root);
  ASSERT_TRUE(service.publish(publication("customer", "ranked", change)));

  const auto ranked = service.query({"alpha", "customer", "", "", 20}, "admin");
  ASSERT_TRUE(ranked) << ranked.error().message;
  ASSERT_EQ(ranked->size(), 2U);
  EXPECT_EQ(ranked->front().path, "first.txt");

  const auto substring =
      service.query({"\"quick brown\"", "customer", "", "", 20}, "admin");
  ASSERT_TRUE(substring) << substring.error().message;
  EXPECT_EQ(substring->size(), 2U);

  const auto regex =
      service.query({"/quick\\s+brown/", "customer", "", "", 20}, "admin");
  ASSERT_TRUE(regex) << regex.error().message;
  EXPECT_EQ(regex->size(), 2U);
}

TEST(Search, RegisteredCapabilityReturnsAuthorizedHits) {
  TemporaryStore store;
  const auto root = store.path();
  const std::string change =
      create_change(root / "customer" / "repositories" / "repository",
                    {{"hit.txt", "registry search fixture"}});
  lake::search::Service service(root);
  ASSERT_TRUE(service.publish(publication("customer", "repository", change)));

  lake::api::Registry registry;
  lake::api::register_core_capabilities(registry, "test");
  lake::api::ApiContext context;
  context.search = &service;
  context.authorized = true;
  context.acting_identity = "admin";
  const auto result = registry.call(
      "search", {{"query", "registry"}, {"customer", "customer"}}, context);

  ASSERT_TRUE(result.ok) << result.error.message;
  ASSERT_EQ(result.value.at("items").size(), 1U);
  EXPECT_EQ(result.value.at("items").front().at("path"), "hit.txt");
}

TEST(SearchBenchmark, DISABLED_BuildAndQueryTenThousandFiles) {
  TemporaryStore store;
  const auto root = store.path();
  const auto repository = root / "customer" / "repositories" / "benchmark";
  auto opened =
      liquid::runtime::block_on(liquid::repository::create(repository));
  ASSERT_TRUE(opened) << opened.error().message;
  auto source = std::move(*opened);
  for (int index = 0; index < 10'000; ++index) {
    const std::string name = "files/" + std::to_string(index) + ".txt";
    const std::string content =
        "benchmark token " + std::to_string(index) + " shared query";
    ASSERT_TRUE(liquid::runtime::block_on(source.write(name, bytes(content))));
  }
  const auto change = liquid::runtime::block_on(source.cut("benchmark corpus"));
  ASSERT_TRUE(change) << change.error().message;

  lake::search::Service service(root);
  const auto build_started = std::chrono::steady_clock::now();
  ASSERT_TRUE(service.publish(
      publication("customer", "benchmark", liquid::to_string(change->value))));
  const auto build_elapsed = std::chrono::steady_clock::now() - build_started;
  const auto query_started = std::chrono::steady_clock::now();
  for (int index = 0; index < 100; ++index) {
    const auto hits =
        service.query({"benchmark", "customer", "", "", 20}, "admin");
    ASSERT_TRUE(hits) << hits.error().message;
  }
  const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
  const auto build_milliseconds =
      std::chrono::duration<double, std::milli>(build_elapsed).count();
  const auto query_microseconds =
      std::chrono::duration<double, std::micro>(query_elapsed).count() / 100.0;
  std::cout << "10k files: index build " << build_milliseconds
            << " ms, mean query " << query_microseconds << " us\n";
}
