#include "lake/control/agent_runs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <print>
#include <vector>

#include <zetta/connect/http_transport.hpp>

namespace lake::control {
namespace {

std::string lowered(std::string_view text) {
    std::string out;
    for (const char character : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return out;
}

std::optional<int> number(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// "HH:MM" to seconds after midnight.
std::optional<std::int64_t> clock_time(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto hours = number(text.substr(0, colon));
    const auto minutes = number(text.substr(colon + 1));
    if (!hours || !minutes || *hours < 0 || *hours > 23 || *minutes < 0 || *minutes > 59) {
        return std::nullopt;
    }
    return *hours * 3600 + *minutes * 60;
}

constexpr std::array<std::string_view, 7> weekdays{
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

struct CronField {
    std::string_view name;
    int low, high;
    std::array<std::string_view, 12> names{}; // three-letter names, from `low`
};

constexpr std::array<CronField, 5> cron_fields{{
    {"minute", 0, 59, {}},
    {"hour", 0, 23, {}},
    {"day of month", 1, 31, {}},
    {"month", 1, 12, {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov",
                      "dec"}},
    {"day of week", 0, 7, {"sun", "mon", "tue", "wed", "thu", "fri", "sat"}},
}};

std::optional<int> cron_value(const CronField& field, std::string_view text) {
    if (const auto value = number(text)) {
        return *value;
    }
    const std::string name = lowered(text);
    for (std::size_t index = 0; index < field.names.size(); ++index) {
        if (!field.names[index].empty() && field.names[index] == name) {
            return field.low + static_cast<int>(index);
        }
    }
    return std::nullopt;
}

// The values one field selects, or why it is refused.
std::expected<std::vector<int>, std::string> cron_values(const CronField& field,
                                                         std::string_view text) {
    std::vector<int> values;
    for (const std::string_view item : split(text, ',')) {
        std::string_view range = item;
        int step = 1;
        if (const std::size_t slash = item.find('/'); slash != std::string_view::npos) {
            const auto parsed = number(item.substr(slash + 1));
            if (!parsed || *parsed < 1) {
                return std::unexpected("the " + std::string(field.name) + " step in '" +
                                       std::string(item) + "' is not a positive number");
            }
            step = *parsed;
            range = item.substr(0, slash);
        }
        int first = field.low, last = field.high;
        if (range != "*") {
            const std::size_t dash = range.find('-');
            const auto from = cron_value(field, range.substr(0, dash));
            const auto to = dash == std::string_view::npos
                                ? (step > 1 ? std::optional<int>(field.high) : from)
                                : cron_value(field, range.substr(dash + 1));
            if (!from || !to || *from < field.low || *to > field.high || *from > *to) {
                return std::unexpected("'" + std::string(item) + "' is not a " +
                                       std::string(field.name) + " (" +
                                       std::to_string(field.low) + "-" +
                                       std::to_string(field.high) + ")");
            }
            first = *from;
            last = *to;
        }
        for (int value = first; value <= last; value += step) {
            values.push_back(value);
        }
    }
    std::ranges::sort(values);
    return values;
}

} // namespace

std::expected<std::string, std::string> normalize_cron(std::string_view expression) {
    std::string text;
    for (const std::string_view part : split(expression, ' ')) {
        if (!part.empty()) {
            text += (text.empty() ? "" : " ") + std::string(part);
        }
    }
    const std::string macro = lowered(text);
    if (macro == "@hourly") {
        return "0 * * * *";
    }
    if (macro == "@daily" || macro == "@midnight") {
        return "0 0 * * *";
    }
    if (macro == "@weekly") {
        return "0 0 * * 0";
    }
    if (macro == "@monthly") {
        return "0 0 1 * *";
    }
    if (macro == "@yearly" || macro == "@annually") {
        return "0 0 1 1 *";
    }
    const auto fields = split(text, ' ');
    if (fields.size() != 5) {
        return std::unexpected(std::string("a cron expression has five fields: minute, hour, day "
                                           "of month, month, day of week"));
    }
    std::vector<int> minutes;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        auto values = cron_values(cron_fields[index], fields[index]);
        if (!values) {
            return std::unexpected(values.error());
        }
        if (index == 0) {
            minutes = std::move(*values);
        }
    }
    // At most once every five minutes: no two minutes of an hour (wrapping to the next) closer.
    for (std::size_t index = 0; index < minutes.size(); ++index) {
        const int next = index + 1 < minutes.size() ? minutes[index + 1] : minutes.front() + 60;
        if (minutes.size() > 1 && next - minutes[index] < 5) {
            return std::unexpected(
                std::string("a schedule may run at most once every five minutes"));
        }
    }
    return text;
}

std::optional<std::string> trigger_cron(std::string_view trigger) {
    const std::string text = lowered(trigger);
    if (text == "hourly") {
        return "0 * * * *";
    }
    if (text == "daily") {
        return "0 0 * * *";
    }
    if (text == "weekly") {
        return "0 0 * * 0";
    }
    if (!text.starts_with("every ")) {
        return std::nullopt;
    }
    std::string_view rest = std::string_view(text).substr(6);
    // "every N minutes|hours|days"
    const std::size_t space = rest.find(' ');
    if (space != std::string_view::npos) {
        if (const auto count = number(rest.substr(0, space)); count && *count > 0) {
            const std::string_view unit = rest.substr(space + 1);
            if (unit.starts_with("minute")) {
                if (*count % 60 == 0 && *count / 60 <= 23) {
                    return *count == 60 ? "0 * * * *" : std::format("0 */{} * * *", *count / 60);
                }
                if (*count < 5 || *count > 59) {
                    return std::nullopt;
                }
                return std::format("*/{} * * * *", *count);
            }
            if (unit.starts_with("hour")) {
                if (*count == 24) {
                    return "0 0 * * *";
                }
                if (*count > 23) {
                    return std::nullopt;
                }
                return *count == 1 ? "0 * * * *" : std::format("0 */{} * * *", *count);
            }
            if (unit.starts_with("day")) {
                if (*count > 31) {
                    return std::nullopt;
                }
                return *count == 1 ? "0 0 * * *" : std::format("0 0 */{} * *", *count);
            }
            return std::nullopt;
        }
    }
    // "every day|weekday|<weekday> at HH:MM"
    const std::size_t at = rest.find(" at ");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view when = rest.substr(0, at);
    const auto time = clock_time(rest.substr(at + 4));
    if (!time) {
        return std::nullopt;
    }
    const std::int64_t hour = *time / 3600, minute = *time % 3600 / 60;
    if (when == "day") {
        return std::format("{} {} * * *", minute, hour);
    }
    if (when == "weekday") {
        return std::format("{} {} * * 1-5", minute, hour);
    }
    for (std::size_t index = 0; index < weekdays.size(); ++index) {
        if (when == weekdays[index]) {
            return std::format("{} {} * * {}", minute, hour, index);
        }
    }
    return std::nullopt;
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
HttpAgentCaller::turn(std::string endpoint, std::string service_token, nlohmann::json body) {
    const zetta::connect::AsyncHttpTransport& transport =
        zetta::connect::default_async_http_transport();
    if (!transport) {
        co_return ZettaControlResult<nlohmann::json>(std::unexpected(
            ZettaControlError{"store_unavailable", "the HTTP transport is not installed"}));
    }
    while (endpoint.ends_with('/')) {
        endpoint.pop_back();
    }
    zetta::connect::HttpRequest request;
    request.method = "POST";
    request.url = endpoint + "/v1/turn";
    request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"}};
    if (!service_token.empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + service_token);
        request.secret_headers = {"Authorization"};
    }
    // An agent turn may call a model several times and run tools; give it room.
    request.timeout_seconds = 300;
    request.body = body.dump();
    auto [transport_error, response] = co_await transport(request);
    if (transport_error || !response.error.empty()) {
        co_return ZettaControlResult<nlohmann::json>(std::unexpected(ZettaControlError{
            "store_unavailable",
            "the agent did not answer" +
                (response.error.empty() ? std::string() : ": " + response.error)}));
    }
    nlohmann::json answer = nlohmann::json::parse(response.body, nullptr, false);
    if (answer.is_discarded() || !answer.is_object()) {
        co_return ZettaControlResult<nlohmann::json>(std::unexpected(ZettaControlError{
            "store_error",
            "the agent answered HTTP " + std::to_string(response.status) + " without JSON"}));
    }
    co_return ZettaControlResult<nlohmann::json>(std::move(answer));
}

} // namespace lake::control
