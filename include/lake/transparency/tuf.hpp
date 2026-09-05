#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lake::transparency {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct Metadata {
    std::string role;
    std::uint64_t version = 0;
    std::string encoded;
    nlohmann::json document;
};

class Service {
public:
    Service(std::filesystem::path store_root, std::string instance_name);

    Result<Metadata> initialize(const nlohmann::json& root);
    Result<Metadata> rotate_root(const nlohmann::json& root);
    Result<nlohmann::json> publish(const nlohmann::json& targets,
                                   const nlohmann::json& snapshot,
                                   const nlohmann::json& timestamp);
    Result<Metadata> metadata(const std::string& role,
                              std::optional<std::uint64_t> version = std::nullopt) const;

    [[nodiscard]] const std::filesystem::path& directory() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::transparency
