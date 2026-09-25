#include "lake/control/zetta_workspace.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

#include <zetta/connect/http_transport.hpp>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> workspace_failure(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

std::string now_rfc3339() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string percent_encode(std::string_view input) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char character : input) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output += static_cast<char>(character);
            continue;
        }
        output += '%';
        output += hex[character >> 4U];
        output += hex[character & 0x0fU];
    }
    return output;
}

std::string text_of(const nlohmann::json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_string() ? found->get<std::string>() : std::string();
}

} // namespace

std::string workspace_slug(std::string_view customer_id) {
    std::string hex;
    for (const char character : customer_id) {
        if (character == '-') {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
            return {};
        }
        hex += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (hex.size() != 32) {
        return {};
    }
    return "ws-" + hex.substr(0, 12);
}

nlohmann::json WorkspaceState::to_json() const {
    return {{"workspace", slug},
            {"requested", requested},
            {"phase", phase},
            {"message", message},
            {"ready", ready()}};
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>> ZettaWorkspaceCaller::call(
    std::string url, std::string token, std::string capability, bool read, nlohmann::json input) {
    const zetta::connect::AsyncHttpTransport& transport =
        zetta::connect::default_async_http_transport();
    if (!transport) {
        co_return workspace_failure<nlohmann::json>("store_unavailable",
                                                    "Zetta HTTP transport is not installed");
    }
    while (url.ends_with('/')) {
        url.pop_back();
    }
    zetta::connect::HttpRequest request;
    request.url = url + "/v1/" + capability;
    request.headers = {{"Accept", "application/json"}, {"Authorization", "Bearer " + token}};
    request.secret_headers = {"Authorization"};
    // A read that hangs must not hold a page for a minute (zetta#1007: get_run_report never
    // answers); actions such as compile_intent may legitimately take longer.
    request.timeout_seconds = read ? 15 : 60;
    if (read) {
        request.method = "GET";
        std::string query;
        for (const auto& [key, value] : input.items()) {
            if (value.is_null()) {
                continue;
            }
            query += query.empty() ? '?' : '&';
            query += percent_encode(key) + "=" +
                     percent_encode(value.is_string() ? value.get<std::string>() : value.dump());
        }
        request.url += query;
    } else {
        request.method = "POST";
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = input.dump();
    }
    auto [transport_error, response] = co_await transport(request);
    if (transport_error || !response.error.empty()) {
        co_return workspace_failure<nlohmann::json>(
            "store_unavailable",
            "the workspace did not answer " + capability +
                (response.error.empty() ? std::string() : ": " + response.error));
    }
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        co_return workspace_failure<nlohmann::json>(
            "store_error",
            "the workspace returned HTTP " + std::to_string(response.status) + " without JSON");
    }
    if (!body.value("ok", false)) {
        const nlohmann::json error = body.value("error", nlohmann::json::object());
        co_return workspace_failure<nlohmann::json>(
            error.value("code", std::string("store_error")),
            error.value("message", "the workspace refused " + capability));
    }
    co_return ZettaControlResult<nlohmann::json>(body.value("value", nlohmann::json::object()));
}

WorkspaceService::WorkspaceService(WorkspaceConfig config,
                                   std::shared_ptr<ZettaControl> control,
                                   std::shared_ptr<WorkspaceCaller> caller)
    : config_(std::move(config)), control_(std::move(control)), caller_(std::move(caller)) {}

std::string WorkspaceService::token_for(std::string_view slug) const {
    std::ifstream input(std::filesystem::path(config_.token_dir) / std::string(slug));
    std::string token((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
        token.pop_back();
    }
    return token;
}

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::state(std::string customer_id) {
    WorkspaceState state;
    state.slug = workspace_slug(customer_id);
    if (state.slug.empty()) {
        co_return workspace_failure<WorkspaceState>("invalid_input", "customer id is not a UUID");
    }
    if (!config_.configured() || !control_) {
        co_return workspace_failure<WorkspaceState>("store_unavailable",
                                                    "workspaces are not configured on this lake");
    }
    const auto quoted_lake = quote_zetta_literal(config_.lake_name);
    if (!quoted_lake) {
        co_return workspace_failure<WorkspaceState>("invalid_input", "invalid workspace identity");
    }
    // Zetta's SQL takes one predicate per WHERE clause (no AND), so filter by Lake in the query
    // and by workspace here, as the operator does.
    auto completed = co_await control_->sql(
        "SELECT workspace_id, record_type, phase, message, updated_at FROM " +
            std::string(workspaces_table) + " WHERE lake = " + *quoted_lake,
        std::string(operator_control_database));
    if (!completed) {
        co_return workspace_failure<WorkspaceState>("store_unavailable", completed.error().message);
    }
    auto rows = std::move(*completed);
    if (!rows) {
        co_return workspace_failure<WorkspaceState>(rows.error().code, rows.error().message);
    }
    std::string latest;
    for (const nlohmann::json& row : *rows) {
        if (text_of(row, "workspace_id") != state.slug) {
            continue;
        }
        const std::string type = text_of(row, "record_type");
        if (type == "desired") {
            state.requested = true;
        } else if (type == "status") {
            const std::string at = text_of(row, "updated_at");
            if (at >= latest) {
                latest = at;
                state.phase = text_of(row, "phase");
                state.message = text_of(row, "message");
            }
        }
    }
    state.credential = !token_for(state.slug).empty();
    co_return ZettaControlResult<WorkspaceState>(std::move(state));
}

liquid::runtime::task<ZettaControlResult<WorkspaceState>>
WorkspaceService::request(std::string customer_id) {
    auto current = co_await state(customer_id);
    if (!current) {
        co_return workspace_failure<WorkspaceState>("store_unavailable", current.error().message);
    }
    if (!*current) {
        co_return workspace_failure<WorkspaceState>(current->error().code,
                                                    current->error().message);
    }
    WorkspaceState result = std::move(**current);
    if (result.requested) {
        co_return ZettaControlResult<WorkspaceState>(std::move(result));
    }
    auto appended = co_await control_->append(std::string(workspaces_table),
                                              {{"workspace_id", result.slug},
                                               {"record_type", "desired"},
                                               {"lake", config_.lake_name},
                                               {"customer_id", customer_id},
                                               {"phase", ""},
                                               {"url", ""},
                                               {"message", ""},
                                               {"updated_at", now_rfc3339()}});
    if (!appended) {
        co_return workspace_failure<WorkspaceState>("store_unavailable", appended.error().message);
    }
    if (!*appended) {
        co_return workspace_failure<WorkspaceState>(appended->error().code,
                                                    appended->error().message);
    }
    result.requested = true;
    co_return ZettaControlResult<WorkspaceState>(std::move(result));
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>> WorkspaceService::call(
    std::string customer_id, std::string capability, bool read, nlohmann::json input) {
    auto current = co_await state(customer_id);
    if (!current) {
        co_return workspace_failure<nlohmann::json>("store_unavailable", current.error().message);
    }
    if (!*current) {
        co_return workspace_failure<nlohmann::json>(current->error().code,
                                                    current->error().message);
    }
    const WorkspaceState& workspace = **current;
    if (!workspace.ready()) {
        co_return workspace_failure<nlohmann::json>("workspace_not_ready",
                                                    workspace.requested
                                                        ? "the workspace is still being prepared"
                                                        : "start the workspace first");
    }
    std::string url = config_.url_template;
    url.replace(url.find("{workspace}"), std::string_view("{workspace}").size(), workspace.slug);
    auto called = co_await caller_->call(
        std::move(url), token_for(workspace.slug), std::move(capability), read, std::move(input));
    if (!called) {
        co_return workspace_failure<nlohmann::json>("store_unavailable", called.error().message);
    }
    co_return std::move(*called);
}

} // namespace lake::control
