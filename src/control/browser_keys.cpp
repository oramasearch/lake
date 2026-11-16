// SPDX-License-Identifier: MIT

// Browser keys (lake#1152, docs/api/browser-keys.md): the origin rule a key's list follows and the
// principal its credential resolves to. The stores keep the rows; the registry checks the binding.

#include <cctype>
#include <string>
#include <string_view>

#include "lake/control/store.hpp"

namespace lake::control {

namespace {

bool host_character(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '.';
}

} // namespace

std::expected<std::string, std::string> browser_key_origin(std::string_view origin) {
    std::string lowered;
    lowered.reserve(origin.size());
    for (const char c : origin) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!lowered.empty() && lowered.back() == '/') {
        lowered.pop_back();
    }
    const bool secure = lowered.starts_with("https://");
    if (!secure && !lowered.starts_with("http://")) {
        return std::unexpected(
            std::string("an origin is the website's address, such as https://www.example.com"));
    }
    const std::string_view rest = std::string_view(lowered).substr(secure ? 8 : 7);
    const std::size_t colon = rest.find(':');
    const std::string_view host = rest.substr(0, colon);
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-' ||
        host.back() == '.' || host.back() == '-' || host.find("..") != std::string_view::npos) {
        return std::unexpected(std::string("the origin has no host name: ") + std::string(origin));
    }
    for (const char c : host) {
        if (!host_character(c)) {
            return std::unexpected(
                std::string("an origin is a scheme and a host, with no path, query or fragment: ") +
                std::string(origin));
        }
    }
    if (colon != std::string_view::npos) {
        const std::string_view port = rest.substr(colon + 1);
        if (port.empty() || port.size() > 5) {
            return std::unexpected(std::string("the origin's port is not a number: ") +
                                   std::string(origin));
        }
        for (const char c : port) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                return std::unexpected(std::string("the origin's port is not a number: ") +
                                       std::string(origin));
            }
        }
        if (std::stoi(std::string(port)) > 65535 || std::stoi(std::string(port)) == 0) {
            return std::unexpected(std::string("the origin's port is out of range: ") +
                                   std::string(origin));
        }
    }
    // Plain HTTP reaches the key only on this computer, for trying the code before it is live.
    if (!secure && host != "localhost" && host != "127.0.0.1") {
        return std::unexpected(std::string("a website's origin starts with https:// (http:// only "
                                           "for localhost and 127.0.0.1): ") +
                               std::string(origin));
    }
    return lowered;
}

Principal browser_key_principal(const BrowserKey& key, std::string digest) {
    std::vector<std::string> scopes{"search_index"};
    if (!key.model_id.empty()) {
        scopes.emplace_back("predict");
    }
    Principal principal{"",
                        "browser_key:" + key.id,
                        "browser_key:" + key.id,
                        "",
                        "lake",
                        "browser_key",
                        std::move(scopes),
                        std::nullopt,
                        key.id,
                        key.customer_id,
                        "",
                        "",
                        std::move(digest)};
    principal.browser_key = key;
    return principal;
}

} // namespace lake::control
