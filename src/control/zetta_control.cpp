#include "lake/control/zetta_control.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <span>
#include <utility>

#include <zetta/api/native_http_transport.hpp>
#include <zetta/connect/http_transport.hpp>

namespace lake::control {
namespace {

std::string percent_encode(std::string_view input) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
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

template <std::size_t Size>
std::span<const std::string_view> column_span(const std::array<std::string_view, Size>& columns) {
    return std::span<const std::string_view>(columns.data(), columns.size());
}

ZettaControlResult<std::span<const std::string_view>> columns_for(std::string_view table) {
    if (table == deployments_table) {
        return column_span(deployment_columns);
    }
    if (table == environment_env_table) {
        return column_span(environment_env_columns);
    }
    if (table == domains_table) {
        return column_span(domain_columns);
    }
    return std::unexpected(
        ZettaControlError{"invalid_input", "unknown operator control table " + std::string(table)});
}

template <class T> ZettaControlResult<T> zetta_failure(ZettaControlError error) {
    return std::unexpected(std::move(error));
}

std::string endpoint_url(std::string endpoint) {
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    return endpoint + "/v1/sql";
}

} // namespace

struct ZettaControlClient::Impl {
    std::string endpoint;
    std::string bearer_token;
};

ZettaControlClient::ZettaControlClient(std::string endpoint, std::string bearer_token)
    : impl_(std::make_unique<Impl>(
          Impl{endpoint_url(std::move(endpoint)), std::move(bearer_token)})) {}

ZettaControlClient::~ZettaControlClient() = default;
ZettaControlClient::ZettaControlClient(ZettaControlClient&&) noexcept = default;
ZettaControlClient& ZettaControlClient::operator=(ZettaControlClient&&) noexcept = default;

ZettaControlResult<std::string> quote_zetta_literal(const nlohmann::json& value) {
    if (value.is_null()) {
        return std::string("NULL");
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? std::string("TRUE") : std::string("FALSE");
    }
    if (value.is_number_integer() || value.is_number_unsigned() || value.is_number_float()) {
        return value.dump();
    }
    if (!value.is_string()) {
        return std::unexpected(
            ZettaControlError{"invalid_input", "operator control values must be scalar"});
    }
    const std::string& text = value.get_ref<const std::string&>();
    std::string output{"'"};
    output.reserve(text.size() + 2);
    for (const unsigned char character : text) {
        if (character < 0x20U || character == 0x7fU) {
            return std::unexpected(ZettaControlError{
                "invalid_input", "operator control values cannot contain control characters"});
        }
        if (character == '\'') {
            output += "''";
        } else {
            output += static_cast<char>(character);
        }
    }
    output += '\'';
    return output;
}

liquid::runtime::task<ZettaControlResult<ZettaRows>> ZettaControlClient::sql(std::string statement,
                                                                             std::string database) {
    const zetta::connect::AsyncHttpTransport& transport =
        zetta::connect::default_async_http_transport();
    if (!transport) {
        co_return zetta_failure<ZettaRows>(
            {"store_unavailable", "Zetta HTTP transport is not installed"});
    }
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = impl_->endpoint + "?statement=" + percent_encode(statement) +
                  "&database=" + percent_encode(database);
    request.headers = {{"Accept", "application/json"},
                       {"Authorization", "Bearer " + impl_->bearer_token}};
    request.secret_headers = {"Authorization"};
    auto [transport_error, response] = co_await transport(request);
    if (transport_error || !response.error.empty()) {
        co_return zetta_failure<ZettaRows>(
            {"store_unavailable",
             response.error.empty() ? "operator Zetta request failed" : response.error});
    }
    if (response.status < 200 || response.status >= 300) {
        // Keep Zetta's own reason (a SQL error, a missing database); it is what makes the failure
        // diagnosable. The request's secret header is never part of it.
        const nlohmann::json failure = nlohmann::json::parse(response.body, nullptr, false);
        const std::string reason =
            failure.is_object()
                ? failure.value("error", nlohmann::json::object()).value("message", std::string())
                : std::string();
        co_return zetta_failure<ZettaRows>({"store_unavailable",
                                            "operator Zetta returned HTTP " +
                                                std::to_string(response.status) +
                                                (reason.empty() ? std::string() : ": " + reason)});
    }
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.value("ok", false)) {
        co_return zetta_failure<ZettaRows>(
            {"store_error", "operator Zetta returned an invalid SQL envelope"});
    }
    const nlohmann::json rows =
        body.value("value", nlohmann::json::object()).value("rows", nlohmann::json::array());
    if (!rows.is_array()) {
        co_return zetta_failure<ZettaRows>(
            {"store_error", "operator Zetta SQL rows are not an array"});
    }
    ZettaRows output;
    output.reserve(rows.size());
    for (const nlohmann::json& row : rows) {
        if (!row.is_object()) {
            co_return zetta_failure<ZettaRows>(
                {"store_error", "operator Zetta returned a non-object row"});
        }
        output.push_back(row);
    }
    co_return ZettaControlResult<ZettaRows>(std::move(output));
}

liquid::runtime::task<ZettaControlResult<void>> ZettaControlClient::append(std::string table,
                                                                           nlohmann::json row) {
    const auto columns = columns_for(table);
    if (!columns) {
        co_return zetta_failure<void>(columns.error());
    }
    std::string names;
    std::string values;
    for (const std::string_view column : *columns) {
        if (!row.contains(column)) {
            co_return zetta_failure<void>(
                {"invalid_input", "missing operator control column " + std::string(column)});
        }
        const auto literal = quote_zetta_literal(row.at(column));
        if (!literal) {
            co_return zetta_failure<void>(literal.error());
        }
        if (!names.empty()) {
            names += ", ";
            values += ", ";
        }
        names += column;
        values += *literal;
    }
    auto completed =
        co_await sql("INSERT INTO " + table + " (" + names + ") VALUES (" + values + ")",
                     std::string(operator_control_database));
    if (!completed) {
        co_return zetta_failure<void>(
            {"store_unavailable",
             "operator Zetta SQL coroutine failed: " + completed.error().message});
    }
    if (!*completed) {
        co_return zetta_failure<void>(completed->error());
    }
    co_return ZettaControlResult<void>{};
}

void install_zetta_control_transport() {
    zetta::api::install_native_http_transport();
}

void shutdown_zetta_control_transport() {
    zetta::api::shutdown_native_http_transport();
}

} // namespace lake::control
