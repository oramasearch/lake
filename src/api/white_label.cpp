// SPDX-License-Identifier: MIT

#include "lake/api/white_label.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace lake::api {
namespace {

std::string lower_trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (out.ends_with('.')) {
        out.pop_back();
    }
    return out;
}

// A DNS host name: labels of letters, digits and inner hyphens, at least two of them.
bool valid_host_name(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.find('.') == std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= host.size()) {
        const std::size_t dot = host.find('.', start);
        const std::string_view label =
            host.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::ranges::all_of(label, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
            })) {
            return false;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return true;
}

std::string without_port(std::string_view host) {
    const std::size_t colon = host.find(':');
    return lower_trimmed(colon == std::string_view::npos ? host : host.substr(0, colon));
}

} // namespace

std::expected<InstanceHosts, std::string> instance_hosts_from_environment(
    std::string_view public_host, std::string_view aliases, std::string_view control_host) {
    InstanceHosts hosts;
    hosts.public_host = std::string(public_host);
    const std::string public_lower = lower_trimmed(public_host);
    std::string_view rest = aliases;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string alias = lower_trimmed(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (alias.empty()) {
            continue;
        }
        if (!valid_host_name(alias)) {
            return std::unexpected("LAKE_HOST_ALIASES: \"" + alias + "\" is not a host name");
        }
        if (alias == public_lower) {
            return std::unexpected("LAKE_HOST_ALIASES: " + alias +
                                   " is LAKE_PUBLIC_HOST itself; an alias is another host");
        }
        if (std::ranges::find(hosts.aliases, alias) == hosts.aliases.end()) {
            hosts.aliases.push_back(alias);
        }
    }
    hosts.control_host = lower_trimmed(control_host);
    if (public_lower.empty() && (!hosts.aliases.empty() || !hosts.control_host.empty())) {
        return std::unexpected(
            "LAKE_HOST_ALIASES and LAKE_CONTROL_HOST need LAKE_PUBLIC_HOST: they are named "
            "against it");
    }
    if (!hosts.control_host.empty() && !valid_host_name(hosts.control_host)) {
        return std::unexpected("LAKE_CONTROL_HOST: \"" + hosts.control_host +
                               "\" is not a host name");
    }
    if (hosts.control_host.empty()) {
        hosts.control_host = hosts.public_host;
    }
    return hosts;
}

bool is_host_alias(const std::vector<std::string>& aliases, std::string_view host) {
    if (aliases.empty()) {
        return false;
    }
    const std::string bare = without_port(host);
    return std::ranges::find(aliases, bare) != aliases.end();
}

std::string alias_redirect_location(std::string_view public_host, std::string_view target) {
    std::string location = "https://" + std::string(public_host);
    // A control character or backslash anywhere can turn a path into another host once a browser
    // reads it (LSR-184, lake#990).
    if (target.starts_with('/') && !target.starts_with("//") && !target.starts_with("/\\") &&
        std::ranges::none_of(target, [](unsigned char c) {
            return c < 0x20 || c == 0x7f || c == '\\';
        })) {
        location += target;
    } else {
        location += '/';
    }
    return location;
}

std::expected<std::string, std::string> sso_sign_in_from_environment(std::string_view provider_id,
                                                                     bool clerk_enabled) {
    const std::string id = lower_trimmed(provider_id);
    if (id.empty()) {
        return std::string{};
    }
    if (clerk_enabled) {
        return std::unexpected(
            "LAKE_SIGN_IN_SSO_PROVIDER and LAKE_CLERK_PUBLISHABLE_KEY are both set: a Lake signs "
            "people in through Clerk or through the customer's own provider, not both");
    }
    if (id.size() > 128 || !std::ranges::all_of(id, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
        })) {
        return std::unexpected("LAKE_SIGN_IN_SSO_PROVIDER: \"" + id +
                               "\" is not a provider id (letters, digits, '-' and '_')");
    }
    // Provider ids are lake's own identifiers; keep the case they were given in.
    std::string kept(provider_id);
    while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.back())) != 0) {
        kept.pop_back();
    }
    while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.front())) != 0) {
        kept.erase(kept.begin());
    }
    return kept;
}

bool names_sso_provider(std::string_view capability) {
    return capability == "start_sso_oidc" || capability == "start_sso_saml" ||
           capability == "sign_in_saml";
}

std::optional<std::string> sso_only_refusal(std::string_view configured_provider,
                                            std::string_view capability,
                                            const nlohmann::json& input,
                                            std::string_view provider_pool) {
    if (configured_provider.empty()) {
        return std::nullopt;
    }
    static constexpr std::array<std::string_view, 8> lake_pool_methods{
        "sign_up_password",
        "sign_in_password",
        "verify_mfa",
        "sign_in_taap",
        "begin_passkey_sign_in",
        "sign_in_passkey",
        "start_oauth",
        "begin_email_recovery",
    };
    if (std::ranges::find(lake_pool_methods, capability) != lake_pool_methods.end()) {
        std::string pool = "lake";
        if (input.is_object()) {
            const auto found = input.find("pool_id");
            if (found != input.end() && found->is_string() && !found->get<std::string>().empty()) {
                pool = found->get<std::string>();
            }
        }
        if (pool != "lake") {
            return std::nullopt;
        }
        return "this Lake signs people in through its organization's identity provider only";
    }
    if (names_sso_provider(capability) && provider_pool == "lake") {
        std::string provider;
        if (input.is_object()) {
            const auto found = input.find("provider_id");
            if (found != input.end() && found->is_string()) {
                provider = found->get<std::string>();
            }
        }
        if (provider != configured_provider) {
            return "this Lake signs people in through its organization's identity provider only";
        }
    }
    return std::nullopt;
}

} // namespace lake::api
