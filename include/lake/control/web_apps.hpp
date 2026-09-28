#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {

// A Ship app's desired and observed state (docs/control-plane/web-apps.md). This is distinct from
// Application, which is an identity user pool (docs/api/apps.md).
struct WebApp {
    std::string id;
    std::string customer_id;
    std::string name;
    std::string mode;
    std::string source_repository;
    std::string source_revision;
    std::string source_path;
    std::string source_entrypoint;
    std::string image;
    std::string framework;
    int port = 0;
    int replicas = 1;
    std::string host;
    std::string auth = "lake";
    nlohmann::json bindings = {{"zetta", nullptr}, {"services", nlohmann::json::array()}};
    nlohmann::json env = nlohmann::json::object();
    nlohmann::json secret_env_refs = nlohmann::json::object();
    nlohmann::json token_hashes = nlohmann::json::object();
    std::string desired = "running";
    std::int64_t generation = 1;
    std::string phase = "Pending";
    std::string url;
    bool ready = false;
    std::string message;
    std::int64_t observed_generation = 0;
    std::string updated_at;
};

struct NewWebApp {
    std::string customer_id;
    std::string name;
    std::string mode;
    std::string source_repository;
    std::string source_revision;
    std::string source_path;
    std::string source_entrypoint;
    std::string image;
    std::string framework;
    int port = 0;
    int replicas = 1;
    std::string host;
    std::string auth = "lake";
    nlohmann::json bindings = {{"zetta", nullptr}, {"services", nlohmann::json::array()}};
    nlohmann::json env = nlohmann::json::object();
    nlohmann::json secret_env_refs = nlohmann::json::object();
    std::size_t max_apps = 0;
    std::size_t max_replicas = 0;
};

// A complete desired-state replacement assembled by a capability from the current row. Store
// implementations lock the row, apply only the fields present here, and increment generation once.
struct WebAppPatch {
    std::optional<std::string> mode;
    std::optional<std::string> source_repository;
    std::optional<std::string> source_revision;
    std::optional<std::string> source_path;
    std::optional<std::string> source_entrypoint;
    std::optional<std::string> image;
    std::optional<int> replicas;
    std::optional<nlohmann::json> bindings;
    std::optional<nlohmann::json> env;
    std::optional<nlohmann::json> secret_env_refs;
    std::optional<std::string> desired;
    std::size_t max_replicas = 0;
    bool rotate_tokens = false;
};

// Returns a purpose only for an active, unambiguous operator hash match.
std::optional<std::string> web_app_token_purpose(const WebApp& app, std::string_view digest);
std::vector<std::string> web_app_token_scopes(std::string_view purpose);

nlohmann::json web_app_json(const WebApp& app);
// The namespace the operator gives one app: app- followed by the first 12 UUID hex digits.
// Empty for an invalid id.
std::string web_app_namespace(std::string_view id);

} // namespace lake::control
