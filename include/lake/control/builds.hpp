// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

struct Build {
    std::string id;
    std::string customer_id;
    std::string workspace_id;
    std::string repository;
    std::string revision;
    std::string context = ".";
    std::string dockerfile = "Dockerfile";
    std::string target;
    nlohmann::json build_args = nlohmann::json::object();
    std::string desired = "queued";
    std::string phase = "Queued";
    std::string digest;
    std::int64_t size = 0;
    std::string message;
    nlohmann::json token_hashes = nlohmann::json::object();
    std::string created_at;
    std::string updated_at;
};

struct NewBuild {
    std::string customer_id;
    std::string workspace_id;
    std::string repository;
    std::string revision;
    std::string context = ".";
    std::string dockerfile = "Dockerfile";
    std::string target;
    nlohmann::json build_args = nlohmann::json::object();
};

struct BuildStatus {
    std::string phase;
    std::string digest;
    std::int64_t size = 0;
    std::string message;
    nlohmann::json token_hashes = nlohmann::json::object();
};

std::optional<std::string> build_token_purpose(const Build& build, std::string_view digest);
std::vector<std::string> build_token_scopes(std::string_view purpose);
nlohmann::json build_json(const Build& build, std::string_view public_host = {});

} // namespace lake::control
