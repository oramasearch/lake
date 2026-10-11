#include "lake/api/web_app.hpp"

#include <array>

namespace lake::api {

namespace {

bool under(std::string_view path, std::string_view prefix) {
    return path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() &&
                              path[prefix.size()] == '/');
}

} // namespace

bool web_app_serves(std::string_view method,
                    std::string_view path,
                    std::string_view accept,
                    bool custom_domain) {
    if (method != "GET" && method != "HEAD") {
        return false;
    }
    if (accept.find("text/html") == std::string_view::npos) {
        return false;
    }
    // Server routes a browser may open directly: the API and its documents, protocol routes,
    // assets, downloads and the hosted application pages.
    static constexpr std::array<std::string_view, 25> server_owned{
        "/v1",
        "/mcp",
        "/ui",
        "/fonts",
        "/apps",
        "/scim",
        "/acme",
        "/v2",
        "/buildid",
        "/_lake",
        "/legacy",
        "/simple",
        "/packages",
        "/api",
        "/healthz",
        "/readyz",
        "/metrics",
        "/skills",
        "/.well-known",
        "/file/raw",
        "/file/download",
        "/drive/download",
        "/exports/download",
        "/developer/mcp/skill",
        "/datasets",
    };
    for (const std::string_view prefix : server_owned) {
        if (under(path, prefix)) {
            return false;
        }
    }
    if (path == "/llms.txt" || path == "/llms-full.txt" || path.starts_with("/style") ||
        path.ends_with("/download") || path.find("/resolve/") != std::string_view::npos ||
        path.find("/zetta/") != std::string_view::npos) {
        return false;
    }
    // A workspace-scoped API call (/w/<id>/v1/..., /w/<id>/mcp) is the API, not a page.
    if (path.starts_with("/w/")) {
        const std::size_t slash = path.find('/', 3);
        if (slash != std::string_view::npos) {
            const std::string_view rest = path.substr(slash);
            if (under(rest, "/v1") || under(rest, "/mcp")) {
                return false;
            }
        }
    }
    if (custom_domain && (path == "/sign-in" || path == "/sign-up" || path == "/mfa" ||
                          path == "/passkey-sign-in")) {
        return false;
    }
    return true;
}

std::string_view web_app_cache_control(std::string_view path) {
    return path.starts_with("/ui/") ? "public, max-age=31536000, immutable" : "no-cache";
}

const WebAppAsset* web_app_asset(std::string_view path) {
    for (const WebAppAsset& asset : web_app_assets()) {
        if (asset.path == path) {
            return &asset;
        }
    }
    return nullptr;
}

std::string_view web_app_index() {
    const WebAppAsset* index = web_app_asset("/index.html");
    return index ? index->body : std::string_view{};
}

} // namespace lake::api
