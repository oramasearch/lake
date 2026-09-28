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
    std::string image;
    std::string framework;
    int port = 0;
    int replicas = 1;
    std::string host;
    std::string auth = "lake";
    nlohmann::json bindings = nlohmann::json::object();
    nlohmann::json env = nlohmann::json::object();
    nlohmann::json secret_env_refs = nlohmann::json::object();
    std::string desired = "running";
    std::int64_t generation = 1;
    std::string phase;
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
    std::string image;
    std::string framework;
    int port = 0;
    int replicas = 1;
    std::string host;
    std::string auth = "lake";
    nlohmann::json bindings = nlohmann::json::object();
    nlohmann::json env = nlohmann::json::object();
    nlohmann::json secret_env_refs = nlohmann::json::object();
    std::string created_by_user_id;
    std::string created_by_taap_id;
    std::string pool_id = "lake";
    std::size_t max_apps = 0;
    std::size_t max_replicas = 0;
};

// A credential retained by lake for operator delivery. The desired app row contains only
// secret_ref; value is returned only through the operator-only credential resolver.
struct WebAppCredential {
    std::string app_id;
    std::string kind;
    std::string secret_ref;
    std::vector<std::string> scopes;
    std::string value;
};

inline constexpr std::string_view web_app_workspace_credential_prefix = "law.";
inline constexpr std::string_view web_app_event_credential_prefix = "lae.";

// A complete desired-state replacement assembled by a capability from the current row. Store
// implementations lock the row, apply only the fields present here, and increment generation once.
struct WebAppPatch {
    std::optional<std::string> mode;
    std::optional<std::string> source_repository;
    std::optional<std::string> source_revision;
    std::optional<std::string> source_path;
    std::optional<std::string> image;
    std::optional<int> replicas;
    std::optional<nlohmann::json> bindings;
    std::optional<nlohmann::json> env;
    std::optional<nlohmann::json> secret_env_refs;
    std::optional<std::string> desired;
    std::size_t max_replicas = 0;
};

nlohmann::json web_app_json(const WebApp& app);
// The namespace the operator gives one app: app- followed by the first 12 UUID hex digits.
// Empty for an invalid id.
std::string web_app_namespace(std::string_view id);

} // namespace lake::control
