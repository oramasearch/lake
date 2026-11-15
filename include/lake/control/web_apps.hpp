// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/store_mounts.hpp"

namespace lake::control {

// A Ship app's desired and observed state (docs/control-plane/web-apps.md). This is distinct from
// Application, which is an identity user pool (docs/api/apps.md).
struct WebApp {
    std::string id;
    std::string customer_id;
    std::string workspace_id;
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
    // The generation the last restart_app wrote, 0 if never (lake#1228). The operator writes it on
    // the pod template, so a restart rolls the pods even when nothing else changed.
    std::int64_t restart_generation = 0;
    // Token rotation as desired state (migration 0112, lake#1070): the generation of tokens Lake
    // wants, which rotation increments, and the generation the operator says `token_hashes` are
    // (nullopt: the operator has not reported one, so it predates this change).
    std::int64_t token_generation = 0;
    std::optional<std::int64_t> token_hashes_generation;

    // Whether `token_hashes` are the tokens Lake wants: always, while the operator reports no
    // generation (Lake then clears the hashes to rotate, as before); otherwise only once the
    // operator has acknowledged the current `token_generation`.
    bool tokens_current() const {
        return !token_hashes_generation || *token_hashes_generation == token_generation;
    }
};

struct NewWebApp {
    std::string customer_id;
    std::string workspace_id;
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
    // The stores the app mounts (migration 0080, lake#666), written in the transaction that
    // creates it; checked by the caller, as NewManagedService::stores.
    std::vector<StoreMountRequest> stores;
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
    std::optional<std::string> auth;
    std::optional<nlohmann::json> bindings;
    std::optional<nlohmann::json> env;
    std::optional<nlohmann::json> secret_env_refs;
    std::optional<std::string> desired;
    // Replaces the stores the app mounts (lake#666). Deleting the app removes them.
    std::optional<std::vector<StoreMountRequest>> stores;
    std::size_t max_replicas = 0;
    bool rotate_tokens = false;
    // restart_app: restart_generation becomes the new generation (lake#1228).
    bool restart = false;
};

struct WebAppSessionClaims {
    std::string app_id;
    std::string user_id;
    std::string workspace_id;
    std::int64_t expires_at = 0;
    std::int64_t issued_at = 0;
};

struct WebAppAuthorizationCode {
    std::string app_id;
    std::string user_id;
    std::string host;
    std::string code_hash;
};

struct WebAppAuthorization {
    WebApp app;
    std::string user_id;
};

// Returns a purpose only for an active, unambiguous operator hash match.
std::optional<std::string> web_app_token_purpose(const WebApp& app, std::string_view digest);
std::vector<std::string> web_app_token_scopes(std::string_view purpose);

nlohmann::json web_app_json(const WebApp& app);
// The namespace the operator gives one app: app- followed by the first 12 UUID hex digits.
// Empty for an invalid id.
std::string web_app_namespace(std::string_view id);

std::expected<std::string, std::string> issue_web_app_session(std::string_view instance_kek,
                                                              std::string_view app_id,
                                                              std::string_view user_id,
                                                              std::string_view workspace_id,
                                                              std::int64_t now_unix = 0);
std::expected<WebAppSessionClaims, std::string> verify_web_app_session(
    std::string_view instance_kek, std::string_view token, std::int64_t now_unix = 0);

} // namespace lake::control
