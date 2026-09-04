#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lake::policy {
class Authorizer;
}

namespace lake::search {

struct Error {
  std::string code;
  std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct Publish {
  std::string customer;
  std::string repository;
  std::string ref;
  std::string change;
};

struct Query {
  std::string text;
  std::string customer;
  std::string repository;
  std::string ref;
  int limit = 20;
};

struct Hit {
  std::string customer;
  std::string repository;
  std::string path;
  std::string change;
  std::string snippet;
  double score = 0.0;
};

class Service {
public:
  explicit Service(std::filesystem::path store_root,
                   std::shared_ptr<const policy::Authorizer> authorizer = {});

  [[nodiscard]] Result<void> publish(const Publish &change);
  [[nodiscard]] Result<std::vector<Hit>>
  query(const Query &query, const std::string &principal) const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace lake::search
