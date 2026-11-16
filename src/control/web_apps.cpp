// SPDX-License-Identifier: MIT

#include "lake/control/web_apps.hpp"

#include <cctype>
#include <chrono>

#include <openssl/crypto.h>

#include "secrets_crypto.hpp"

namespace lake::control {

namespace {

constexpr std::int64_t web_app_session_lifetime_seconds = 12 * 60 * 60;
constexpr std::string_view web_app_session_hkdf_label = "lake/app-session/v1";

nlohmann::json nullable(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::expected<secrets_crypto::Key, std::string> web_app_session_key(std::string_view instance_kek) {
    auto root = secrets_crypto::instance_key(instance_kek);
    if (!root) {
        return std::unexpected(root.error().message);
    }
    auto derived = secrets_crypto::derive_hkdf(*root, web_app_session_hkdf_label);
    secrets_crypto::cleanse(*root);
    if (!derived) {
        return std::unexpected(derived.error().message);
    }
    return *derived;
}

} // namespace

std::optional<std::string> web_app_token_purpose(const WebApp& app, std::string_view digest) {
    // A rotation the operator has not acknowledged yet revokes every token at once (lake#1070).
    if (digest.size() != 64 || app.phase == "Deleted" || !app.tokens_current()) {
        return std::nullopt;
    }
    std::optional<std::string> matched;
    for (const std::string purpose : {"source", "zetta", "events", "pull"}) {
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
    if (purpose == "pull") {
        return {"registry_pull"};
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
            {"workspace_id", app.workspace_id},
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
            {"restart_generation", app.restart_generation},
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

std::expected<std::string, std::string> issue_web_app_session(std::string_view instance_kek,
                                                              std::string_view app_id,
                                                              std::string_view user_id,
                                                              std::string_view workspace_id,
                                                              std::int64_t now_unix) {
    if (app_id.empty() || user_id.empty() || workspace_id.empty()) {
        return std::unexpected("app-session identity fields are required");
    }
    if (now_unix == 0) {
        now_unix = unix_now();
    }
    const nlohmann::json claims{{"app_id", app_id},
                                {"user_id", user_id},
                                {"workspace_id", workspace_id},
                                {"exp", now_unix + web_app_session_lifetime_seconds},
                                {"iat", now_unix}};
    auto payload = secrets_crypto::base64url_encode(claims.dump());
    if (!payload) {
        return std::unexpected(payload.error().message);
    }
    auto key = web_app_session_key(instance_kek);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto signature = secrets_crypto::hmac(*key, *payload);
    secrets_crypto::cleanse(*key);
    if (!signature) {
        return std::unexpected(signature.error().message);
    }
    return *payload + "." + *signature;
}

std::expected<WebAppSessionClaims, std::string> verify_web_app_session(
    std::string_view instance_kek, std::string_view token, std::int64_t now_unix) {
    const std::size_t separator = token.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size() ||
        token.find('.', separator + 1) != std::string_view::npos) {
        return std::unexpected("app session is malformed");
    }
    const std::string_view payload = token.substr(0, separator);
    const std::string_view signature = token.substr(separator + 1);
    auto key = web_app_session_key(instance_kek);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto expected = secrets_crypto::hmac(*key, payload);
    secrets_crypto::cleanse(*key);
    if (!expected || expected->size() != signature.size() ||
        CRYPTO_memcmp(expected->data(), signature.data(), signature.size()) != 0) {
        return std::unexpected("app session signature is invalid");
    }
    auto decoded = secrets_crypto::base64url_decode(payload);
    if (!decoded) {
        return std::unexpected("app session payload is invalid");
    }
    const nlohmann::json claims = nlohmann::json::parse(*decoded, nullptr, false);
    if (claims.is_discarded() || !claims.is_object() || claims.size() != 5 ||
        !claims.contains("app_id") || !claims.at("app_id").is_string() ||
        !claims.contains("user_id") || !claims.at("user_id").is_string() ||
        !claims.contains("workspace_id") || !claims.at("workspace_id").is_string() ||
        !claims.contains("exp") || !claims.at("exp").is_number_integer() ||
        !claims.contains("iat") || !claims.at("iat").is_number_integer()) {
        return std::unexpected("app session claims are invalid");
    }
    WebAppSessionClaims verified{claims.at("app_id").get<std::string>(),
                                 claims.at("user_id").get<std::string>(),
                                 claims.at("workspace_id").get<std::string>(),
                                 claims.at("exp").get<std::int64_t>(),
                                 claims.at("iat").get<std::int64_t>()};
    if (verified.app_id.empty() || verified.user_id.empty() || verified.workspace_id.empty()) {
        return std::unexpected("app session claims are invalid");
    }
    if (now_unix == 0) {
        now_unix = unix_now();
    }
    if (verified.issued_at > now_unix || verified.expires_at <= now_unix ||
        verified.expires_at - verified.issued_at != web_app_session_lifetime_seconds) {
        return std::unexpected("app session is expired or has an invalid lifetime");
    }
    return verified;
}

} // namespace lake::control
