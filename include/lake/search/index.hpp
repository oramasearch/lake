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

enum class QueryMode { Hot, Snapshot, Scan };

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
    [[nodiscard]] Result<QueryResult>
    query(const Query& query, const std::string& principal, control::Store& store) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::search
