// SPDX-License-Identifier: MIT

#include "lake/api/web_app.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <string>

#include <openssl/evp.h>

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
    static constexpr std::array<std::string_view, 26> server_owned{
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
        "/tunnel-clients",
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

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The value of attribute `name` in the start tag `tag` (lowercased), or nullopt when absent.
std::optional<std::string> attribute(std::string_view tag, std::string_view name) {
    std::size_t at = 0;
    while ((at = tag.find(name, at)) != std::string_view::npos) {
        const bool starts = at > 0 && (std::isspace(static_cast<unsigned char>(tag[at - 1])) != 0);
        std::size_t after = at + name.size();
        if (!starts || (after < tag.size() && tag[after] != '=' && tag[after] != ' ' &&
                        tag[after] != '>' && tag[after] != '/')) {
            at = after;
            continue;
        }
        while (after < tag.size() && tag[after] == ' ') {
            ++after;
        }
        if (after >= tag.size() || tag[after] != '=') {
            return std::string();
        }
        ++after;
        while (after < tag.size() && tag[after] == ' ') {
            ++after;
        }
        if (after < tag.size() && (tag[after] == '"' || tag[after] == '\'')) {
            const char quote = tag[after];
            const std::size_t end = tag.find(quote, after + 1);
            return std::string(tag.substr(after + 1,
                                          end == std::string_view::npos ? std::string_view::npos
                                                                        : end - after - 1));
        }
        std::size_t end = after;
        while (end < tag.size() && tag[end] != ' ' && tag[end] != '>') {
            ++end;
        }
        return std::string(tag.substr(after, end - after));
    }
    return std::nullopt;
}

bool runs_as_script(const std::optional<std::string>& type) {
    if (!type || type->empty()) {
        return true;
    }
    const std::string value = lower(*type);
    return value == "module" || value == "text/javascript" || value == "application/javascript" ||
           value == "text/ecmascript" || value == "application/ecmascript";
}

std::string sha256_base64(std::string_view text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &size, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), digest, static_cast<int>(size));
    out.resize(written < 0 ? 0 : static_cast<std::size_t>(written));
    return out;
}

} // namespace

std::string inline_script_hashes(std::string_view html) {
    const std::string folded = lower(html);
    std::string hashes;
    std::size_t at = 0;
    while ((at = folded.find("<script", at)) != std::string::npos) {
        const std::size_t tag_end = folded.find('>', at);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string_view tag = std::string_view(folded).substr(at, tag_end - at);
        const std::size_t close = folded.find("</script", tag_end + 1);
        if (close == std::string::npos) {
            break;
        }
        if (!attribute(tag, "src") && runs_as_script(attribute(tag, "type"))) {
            // Hashed as the browser reads it: the element's text, byte for byte, in its own case.
            const std::string hash = sha256_base64(html.substr(tag_end + 1, close - tag_end - 1));
            if (!hash.empty() && hashes.find(hash) == std::string::npos) {
                hashes += " 'sha256-" + hash + "'";
            }
        }
        at = close + 1;
    }
    return hashes;
}

std::string web_app_content_security_policy(std::string_view html,
                                            std::string_view clerk_frontend_api) {
    const std::string clerk =
        clerk_frontend_api.empty() ? std::string() : " https://" + std::string(clerk_frontend_api);
    std::string policy = "default-src 'self'; script-src 'self'" + inline_script_hashes(html);
    if (!clerk.empty()) {
        policy += clerk + " https://challenges.cloudflare.com";
    }
    policy += "; style-src 'self' 'unsafe-inline'; img-src 'self' data:";
    if (!clerk.empty()) {
        policy += " https://img.clerk.com";
    }
    policy += "; font-src 'self'; connect-src 'self'" + clerk +
              "; media-src 'self' blob:; worker-src 'self' blob:; frame-src 'self' https:; "
              "frame-ancestors 'self'; base-uri 'none'; object-src 'none'; form-action 'self'";
    return policy;
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
