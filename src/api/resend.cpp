// SPDX-License-Identifier: MIT

// Resend (lake#831, docs/api/orders.md#email): lake's own client for Resend's REST API. No Resend
// SDK; the HTTP exchange is injected (ResendHttp), the server's outbound client in lake.

#include "lake/api/resend.hpp"

#include <algorithm>
#include <format>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

namespace lake::api {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// The address of a From value: "Name <address>" or a bare address.
std::string_view from_address(std::string_view from) {
    from = trim(from);
    const std::size_t open = from.rfind('<');
    if (open != std::string_view::npos && from.ends_with('>')) {
        return from.substr(open + 1, from.size() - open - 2);
    }
    return from;
}

} // namespace

std::string ResendSettings::unavailable_reason() const {
    if (api_key.empty()) {
        return "LAKE_RESEND_API_KEY is not set";
    }
    if (from.empty()) {
        return "LAKE_RESEND_FROM is not set";
    }
    if (support_email.empty()) {
        return "LAKE_SUPPORT_EMAIL is not set";
    }
    return {};
}

bool plausible_email_address(std::string_view address) {
    if (address.size() < 3 || address.size() > 254) {
        return false;
    }
    const std::size_t at = address.find('@');
    if (at == std::string_view::npos || at == 0 ||
        address.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view domain = address.substr(at + 1);
    const std::size_t dot = domain.find('.');
    if (dot == std::string_view::npos || dot == 0 || domain.back() == '.') {
        return false;
    }
    return std::ranges::none_of(address, [](unsigned char c) {
        return c <= ' ' || c == 0x7f || c == '<' || c == '>' || c == ',' || c == ';' || c == '"' ||
               c == '\\' || c == '(' || c == ')';
    });
}

std::expected<ResendSettings, std::string> resend_settings_from_environment(
    std::string_view api_key, std::string_view from, std::string_view support_email) {
    ResendSettings settings;
    settings.api_key = std::string(trim(api_key));
    if (!settings.api_key.empty() &&
        (!settings.api_key.starts_with("re_") || settings.api_key.size() < 8 ||
         settings.api_key.size() > 255 ||
         !std::ranges::all_of(settings.api_key, [](char c) { return c > ' ' && c < 0x7f; }))) {
        return std::unexpected(std::string("LAKE_RESEND_API_KEY is not a Resend API key (re_...)"));
    }
    if (!trim(from).empty()) {
        settings.from = std::string(trim(from));
    }
    if (!settings.from.empty() && (!plausible_email_address(from_address(settings.from)) ||
                                   settings.from.find_first_of("\r\n") != std::string::npos)) {
        return std::unexpected(std::format(
            "LAKE_RESEND_FROM must be an email address or \"Name <address>\", not \"{}\"",
            settings.from));
    }
    if (!trim(support_email).empty()) {
        settings.support_email = std::string(trim(support_email));
    }
    if (!settings.support_email.empty() && !plausible_email_address(settings.support_email)) {
        return std::unexpected(std::format(
            "LAKE_SUPPORT_EMAIL must be an email address, not \"{}\"", settings.support_email));
    }
    return settings;
}

std::expected<std::string, std::string> send_resend_email(const ResendSettings& settings,
                                                          const Email& email,
                                                          std::string_view idempotency_key,
                                                          const ResendHttp& http,
                                                          std::string_view base) {
    if (settings.api_key.empty()) {
        return std::unexpected(settings.unavailable_reason());
    }
    if (!http) {
        return std::unexpected(std::string("this Lake has no outbound HTTP client for Resend"));
    }
    if (email.to.empty() || !std::ranges::all_of(email.to, [](const std::string& to) {
            return plausible_email_address(to);
        })) {
        return std::unexpected(std::string("the email has no valid recipient"));
    }
    nlohmann::json body{{"from", settings.from},
                        {"to", email.to},
                        {"subject", email.subject},
                        {"text", email.text}};
    if (!email.reply_to.empty()) {
        body["reply_to"] = email.reply_to;
    }
    ResendRequest request;
    request.url = std::string(base) + "/emails";
    // Invalid UTF-8 in what a person typed is replaced, never a reason to lose the email.
    request.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    request.headers = {{"Authorization", "Bearer " + settings.api_key},
                       {"Content-Type", "application/json"},
                       {"Idempotency-Key", std::string(idempotency_key)}};
    const ResendResponse response = http(request);
    if (!response.error.empty()) {
        return std::unexpected("Resend could not be reached: " + response.error);
    }
    if (response.status == 401 || response.status == 403) {
        return std::unexpected(std::format(
            "Resend refused Lake's key (HTTP {}); check LAKE_RESEND_API_KEY", response.status));
    }
    const nlohmann::json answer = nlohmann::json::parse(response.body, nullptr, false);
    if (response.status < 200 || response.status >= 300) {
        std::string message = answer.is_object() ? answer.value("message", "") : "";
        if (message.empty()) {
            message = "no message";
        }
        return std::unexpected(std::format(
            "Resend refused the email (HTTP {}): {}", response.status, control::utf8_prefix(message, 300)));
    }
    if (!answer.is_object() || !answer.contains("id") || !answer["id"].is_string()) {
        return std::unexpected(std::string("Resend answered without an email id"));
    }
    return answer["id"].get<std::string>();
}

} // namespace lake::api
