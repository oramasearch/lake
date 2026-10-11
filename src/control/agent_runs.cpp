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
            return std::unexpected("'" + text +
                                   "' runs more often than lake allows: a schedule may run at "
                                   "most once every five minutes (the shortest is */5 * * * *)");
        }
    }
    return text;
}

std::expected<std::string, std::string> trigger_cron(std::string_view trigger) {
    const std::string text = lowered(trigger);
    const std::string quoted = "'" + std::string(trigger) + "'";
    const std::string understood =
        "; use every N minutes (5 to 59, or whole hours), every N hours or days, hourly, daily, "
        "weekly, every day|weekday|<weekday> at HH:MM, or a cron expression";
    const auto unplaceable = [&] {
        return std::unexpected("lake cannot place " + quoted + " in time" + understood);
    };
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
        return unplaceable();
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
                if (*count < 5) {
                    return std::unexpected(
                        quoted + " is too often: a schedule runs at most once every five "
                                 "minutes, so the shortest is 'every 5 minutes'");
                }
                if (*count > 59) {
                    return std::unexpected(
                        quoted + " cannot be a cron schedule: minutes go from 5 to 59, or a "
                                 "whole number of hours such as 'every 120 minutes'");
                }
                return std::format("*/{} * * * *", *count);
            }
            if (unit.starts_with("hour")) {
                if (*count == 24) {
                    return "0 0 * * *";
                }
                if (*count > 23) {
                    return std::unexpected(quoted + " cannot be a cron schedule: hours go from "
                                                    "1 to 24; use every N days for longer");
                }
                return *count == 1 ? "0 * * * *" : std::format("0 */{} * * *", *count);
            }
            if (unit.starts_with("day")) {
                if (*count > 31) {
                    return std::unexpected(quoted + " cannot be a cron schedule: days go from 1 "
                                                    "to 31; use a cron expression for longer");
                }
                return *count == 1 ? "0 0 * * *" : std::format("0 0 */{} * *", *count);
            }
            return unplaceable();
        }
    }
    // "every day|weekday|<weekday> at HH:MM"
    const std::size_t at = rest.find(" at ");
    if (at == std::string_view::npos) {
        return unplaceable();
    }
    const std::string_view when = rest.substr(0, at);
    const auto time = clock_time(rest.substr(at + 4));
    if (!time) {
        return std::unexpected(quoted + " has no time of day lake can read; write it as HH:MM, "
                                        "such as 09:00");
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
    return unplaceable();
}

std::expected<std::string, std::string> checked_time_zone(std::string_view name) {
    const auto& names = iana_time_zones();
    if (std::ranges::binary_search(names, name)) {
        return std::string(name);
    }
    const std::string quoted = "'" + std::string(name) + "'";
    const std::string folded = lowered(name);
    for (const std::string_view known : names) {
        if (lowered(known) == folded) {
            return std::unexpected(quoted + " is not an IANA time zone; did you mean '" +
                                   std::string(known) + "'? (the name is case-sensitive)");
        }
    }
    return std::unexpected(quoted + " is not an IANA time zone; give one such as Etc/UTC, "
                                    "America/Toronto or Europe/Paris");
}

AgentScheduleProgress agent_schedule_progress(const AgentCronSchedule& schedule) {
    if (schedule.desired != "present" && (schedule.phase.empty() || schedule.phase == "Stopped")) {
        return {"Stopped", "Removed: it runs no more."};
    }
    if (schedule.phase.empty() && schedule.observed_generation == 0) {
        return {"Pending", "Waiting for lake to set up this schedule."};
    }
    if (schedule.phase == "Ready") {
        return {"Ready", "Scheduled."};
    }
    if (schedule.phase != "Failed") {
        return {schedule.phase, schedule.message};
    }
    // The operator's message names Kubernetes objects and fields; a person gets what to do.
    const std::string not_scheduled = "Not scheduled, so it will not run. ";
    if (!checked_time_zone(schedule.time_zone)) {
        return {"Failed",
                not_scheduled + "Lake does not know the time zone '" + schedule.time_zone +
                    "'. Remove this schedule and add it again with an IANA zone such as Etc/UTC."};
    }
    if (schedule.message.find("not enabled") != std::string::npos) {
        return {"Failed",
                not_scheduled + "Unattended agent runs are not enabled on this lake; ask the "
                                "person who runs it to turn them on."};
    }
    return {"Failed",
            not_scheduled + "Lake could not set it up. Remove it and add it again; if it fails "
                            "again, ask the person who runs this lake."};
}

std::optional<std::chrono::sys_seconds> utc_seconds(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SSZ, as both stores write created_at.
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        return std::nullopt;
    }
    const auto year = number(text.substr(0, 4));
    const auto month = number(text.substr(5, 2));
    const auto day = number(text.substr(8, 2));
    const auto hour = number(text.substr(11, 2));
    const auto minute = number(text.substr(14, 2));
    const auto second = number(text.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year(*year),
                                           std::chrono::month(static_cast<unsigned>(*month)),
                                           std::chrono::day(static_cast<unsigned>(*day))};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    return std::chrono::sys_days(date) + std::chrono::hours(*hour) +
           std::chrono::minutes(*minute) + std::chrono::seconds(*second);
}

AgentLaunchProgress agent_launch_progress(const AgentLaunch& launch,
                                          std::chrono::sys_seconds now) {
    if (!launch.stop_reason.empty() && (launch.phase.empty() || launch.phase == "Stopped")) {
        return {"Failed", launch.stop_reason};
    }
    if (!launch.phase.empty() || launch.observed_generation != 0) {
        return {launch.phase, launch.message};
    }
    if (launch.desired != "present") {
        return {"Stopped", "Stopped before the Orama operator started it."};
    }
    const auto created = utc_seconds(launch.created_at);
    const std::int64_t waited = created ? (now - *created).count() : 0;
    if (waited >= agent_launch_start_timeout_seconds) {
        return {"Failed", std::string(agent_launch_start_timeout_reason)};
    }
    if (waited < 60) {
        return {"Pending", "Waiting for the Orama operator to start this run."};
    }
    return {"Pending",
            std::format("The Orama operator has not started this run after {} seconds. Lake "
                        "stops it with a reason at five minutes.",
                        waited)};
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
