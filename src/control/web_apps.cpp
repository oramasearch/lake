#include "lake/control/web_apps.hpp"

#include <cctype>

#include <openssl/crypto.h>

namespace lake::control {

namespace {

nlohmann::json nullable(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

} // namespace

std::optional<std::string> web_app_token_purpose(const WebApp& app, std::string_view digest) {
    if (digest.size() != 64 || app.phase == "Deleted") {
        return std::nullopt;
    }
    std::optional<std::string> matched;
    for (const std::string purpose : {"source", "zetta", "events"}) {
        const auto hash = app.token_hashes.find(purpose);
        if (hash == app.token_hashes.end() || !hash->is_string()) {
            continue;
        }
        const std::string& expected = hash->get_ref<const std::string&>();
        if (expected.size() != digest.size() ||
            CRYPTO_memcmp(expected.data(), digest.data(), digest.size()) != 0) {
            continue;
        }
        if (matched) {
            return std::nullopt;
        }
        matched = purpose;
    }
    if (!matched || (app.desired == "deleted" && *matched != "events")) {
        return std::nullopt;
    }
    if (*matched == "source" &&
        (app.mode != "runtime" || app.source_repository.empty() || app.source_revision.empty())) {
        return std::nullopt;
    }
    if (*matched == "zetta" && app.bindings.value("zetta", nlohmann::json()) !=
                                   nlohmann::json{{"scopes", nlohmann::json::array({"read"})}}) {
        return std::nullopt;
    }
    return matched;
}

std::vector<std::string> web_app_token_scopes(std::string_view purpose) {
    if (purpose == "source") {
        return {"repository_tree", "repository_file"};
    }
    if (purpose == "zetta") {
        return {"workspace_query", "workspace_sql"};
    }
    if (purpose == "events") {
        return {"publish_app_event"};
    }
    return {};
}

nlohmann::json web_app_json(const WebApp& app) {
    nlohmann::json source = nullptr;
    if (app.mode == "runtime") {
        source = {{"repository", app.source_repository},
                  {"revision", app.source_revision},
                  {"path", app.source_path},
                  {"entrypoint", app.source_entrypoint}};
    }
    return {{"id", app.id},
            {"customer_id", app.customer_id},
            {"name", app.name},
            {"mode", app.mode},
            {"source", std::move(source)},
            {"image", nullable(app.image)},
            {"framework", app.framework},
            {"port", app.port},
            {"replicas", app.replicas},
            {"host", app.host},
            {"auth", app.auth},
            {"bindings", app.bindings},
            {"env", app.env},
            {"secret_env_refs", app.secret_env_refs},
            {"desired", app.desired},
            {"generation", app.generation},
            {"phase", app.phase},
            {"url", nullable(app.url)},
            {"ready", app.ready},
            {"message", app.message},
            {"observed_generation", app.observed_generation},
            {"updated_at", app.updated_at}};
}

std::string web_app_namespace(std::string_view id) {
    std::string hex;
    for (const char character : id) {
        if (character == '-') {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
            return {};
        }
        hex += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return hex.size() == 32 ? "app-" + hex.substr(0, 12) : std::string();
}

} // namespace lake::control
