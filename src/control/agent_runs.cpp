#include "lake/control/agent_runs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <print>

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

} // namespace

std::optional<std::int64_t> next_run(std::string_view trigger, std::int64_t now_unix) {
    const std::string text = lowered(trigger);
    constexpr std::int64_t day = 86400;
    if (text == "hourly") {
        return now_unix + 3600;
    }
    if (text == "daily") {
        return now_unix + day;
    }
    if (text == "weekly") {
        return now_unix + 7 * day;
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
                return now_unix + std::max<std::int64_t>(*count, 5) * 60;
            }
            if (unit.starts_with("hour")) {
                return now_unix + *count * 3600;
            }
            if (unit.starts_with("day")) {
                return now_unix + *count * day;
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
    const std::int64_t midnight = now_unix - now_unix % day;
    const int today = static_cast<int>(((now_unix / day) + 4) % 7); // 1970-01-01 was a Thursday
    for (int offset = 0; offset <= 7; ++offset) {
        const std::int64_t candidate = midnight + offset * day + *time;
        if (candidate <= now_unix) {
            continue;
        }
        const int weekday = (today + offset) % 7;
        if (when == "day" || (when == "weekday" && weekday >= 1 && weekday <= 5) ||
            when == weekdays[weekday]) {
            return candidate;
        }
    }
    return std::nullopt;
}

AgentScheduler::AgentScheduler(std::shared_ptr<Store> store, std::chrono::seconds interval)
    : store_(std::move(store)), interval_(interval) {}

AgentScheduler::~AgentScheduler() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AgentScheduler::start() {
    thread_ = std::thread([this] {
        while (!stopping_) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            tick(now);
            for (std::int64_t waited = 0; waited < interval_.count() && !stopping_; ++waited) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
            }
        }
    });
}

std::size_t AgentScheduler::tick(std::int64_t now_unix) {
    const auto due = store_->due_agent_schedules(now_unix);
    if (!due) {
        std::println(stderr, "lake: agent scheduler: {}", due.error().message);
        return 0;
    }
    std::size_t fired = 0;
    for (const AgentSchedule& schedule : *due) {
        const auto launched = store_->create_agent_launch(NewAgentLaunch{
            .customer_id = schedule.customer_id,
            .agent_id = schedule.agent_id,
            .agent_name = schedule.agent_name,
            .mode = "job",
            .trigger = "schedule",
            .input = {{"trigger", schedule.trigger}, {"scheduled_for", schedule.next_run_unix}},
            .scopes = agent_launch_scopes,
            .created_by_user_id = schedule.created_by_user_id,
            .created_by_taap_id = schedule.created_by_taap_id});
        if (launched) {
            ++fired;
        } else {
            std::println(stderr, "lake: agent scheduler: {}", launched.error().message);
        }
        // A trigger lake can no longer place in time stops rather than firing every tick.
        const auto next = next_run(schedule.trigger, now_unix);
        store_->mark_agent_schedule_run(schedule.customer_id,
                                        schedule.agent_id,
                                        now_unix,
                                        next.value_or(now_unix + 3650 * 86400));
    }
    return fired;
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
