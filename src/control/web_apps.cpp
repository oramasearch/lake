#include "lake/control/web_apps.hpp"

#include <cctype>

namespace lake::control {

namespace {

nlohmann::json nullable(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

} // namespace

nlohmann::json web_app_json(const WebApp& app) {
    nlohmann::json source = nullptr;
    if (app.mode == "source") {
        source = {{"repository", app.source_repository},
                  {"revision", app.source_revision},
                  {"path", app.source_path}};
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
