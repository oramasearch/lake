#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lake::control {
class Store;
}

namespace lake::search {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

enum class PublicationKind { BranchTip, TagSnapshot };

enum class QueryMode { Hot, Snapshot, Scan, Artifact };

[[nodiscard]] std::string_view to_string(QueryMode mode);

struct Publish {
    std::string customer;
    std::string repository;
    std::string ref;
    std::string change;
    PublicationKind kind = PublicationKind::BranchTip;
};

struct Query {
    std::string text;
    std::string customer;
    std::string repository;
    std::string ref;
    int limit = 20;
    std::string change;
    bool history = false;
};

struct Hit {
    std::string customer;
    std::string repository;
    std::string path;
    std::string change;
    std::string snippet;
    double score = 0.0;
    std::string artifact;
    std::string artifact_version;
    std::string digest;
    std::size_t line = 0;
};

struct ArtifactMember {
    std::string artifact;
    std::string version;
    std::string path;
    std::string digest;
    bool indexed = false;
    std::string reason;
};

struct ReindexResult {
    std::size_t artifacts = 0;
    std::size_t members = 0;
    std::size_t indexed = 0;
    std::size_t binary = 0;
    std::size_t truncated = 0;
};

struct QueryResult {
    QueryMode mode = QueryMode::Hot;
    std::size_t scanned_changes = 0;
    std::vector<Hit> items;
};

class Service {
public:
    explicit Service(std::filesystem::path store_root);

    [[nodiscard]] Result<void> publish(const Publish& change);
    [[nodiscard]] Result<ReindexResult> reindex_artifacts(const std::string& customer,
                                                          const std::string& repository);
    [[nodiscard]] Result<std::vector<ArtifactMember>>
    artifact_members(const std::string& customer,
                     const std::string& repository,
                     const std::string& version) const;
    [[nodiscard]] Result<QueryResult>
    query(const Query& query, const std::string& principal, control::Store& store) const;
    // Drops one repository's search segments and artifact members, and the indexed blobs nothing
    // else references (a workspace deletion, lake#514). Dropping a repository with none is a
    // success, so a resumed deletion can repeat it.
    [[nodiscard]] Result<void> drop_repository(const std::string& customer,
                                               const std::string& repository);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::search
