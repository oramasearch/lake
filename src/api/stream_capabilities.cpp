#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <set>

#include "lake/control/agent_bus.hpp"
#include "lake/control/zetta_workspace.hpp"

namespace lake::api {
namespace {
using nlohmann::json;
using Task = liquid::runtime::task<ApiResult>;

json object(json properties, json required = json::array()) {
    return {{"type", "object"}, {"properties", std::move(properties)},
            {"required", std::move(required)}, {"additionalProperties", false}};
}
json text() {
    return {{"type", "string"}};
}
json integer(std::int64_t minimum = 0) {
    return {{"type", "integer"}, {"minimum", minimum}};
}
json flag() {
    return {{"type", "boolean"}};
}
json array(json item) {
    return {{"type", "array"}, {"items", std::move(item)}};
}
json choice(json values, bool nullable = false) {
    if (nullable) {
        values.push_back(nullptr);
    }
    return {{"type", nullable ? json({"string", "null"}) : json("string")},
            {"enum", std::move(values)}};
}
json optional_number(std::int64_t minimum = 0) {
    return {{"type", {"integer", "null"}}, {"minimum", minimum}};
}
json optional_text() {
    return {{"type", {"string", "null"}}};
}

json settings_schema(bool mutable_only = false) {
    const auto guarantee = choice({"at_most_once", "at_least_once", "exactly_once"}, true);
    json properties = {
        {"retention_ms", optional_number()}, {"max_events", optional_number()},
        {"default_ttl_ms", optional_number(1)}, {"memory_cap_bytes", optional_number()},
        {"disk_cap_bytes", optional_number()},
        {"overflow", choice({"unbounded", "skip_ahead", "disconnect", "hold_publisher",
                              "reject_new", "spill_to_disk"}, true)},
        {"over_bound", choice({"clamp", "reject"})},
        {"dispatch", choice({"inline", "per_key", "auto", "pool"}, true)},
        {"priority_aging_ms", integer()}, {"topics_chain_max", integer(1)},
        {"delivery", object({
            {"recipients", choice({"all", "one", "count"}, true)},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295LL}}},
            {"ordering", choice({"fifo", "priority"}, true)},
            {"priority", {{"type", {"integer", "null"}}, {"minimum", 0}, {"maximum", 255}}},
            {"guarantee", guarantee}, {"ack_deadline_ms", optional_number(1)},
            {"reply", choice({"none", "reply_to", "reply_all"}, true)}})},
        {"bounds", object({
            {"max_priority", {{"type", {"integer", "null"}}, {"minimum", 0}, {"maximum", 255}}},
            {"max_recipients", {{"type", {"integer", "null"}}, {"minimum", 1},
                                {"maximum", 4294967295LL}}},
            {"max_guarantee", guarantee},
            {"allowed_recipient_modes", array(choice({"all", "one", "count"}))}})}};
    if (!mutable_only) {
        properties.update({
            {"lifecycle", choice({"default", "ephemeral"})},
            {"store", choice({"ring_stack", "queue"})},
            {"ring_capacity", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1048576}}},
            {"inline_threshold", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1048576}}},
            {"priority_levels", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}},
            {"producer_queue", flag()}, {"exposed", flag()}});
    }
    return object(std::move(properties));
}

json topic_schema() {
    const auto subscription = object(
        {{"id", integer()}, {"subscriber", text()}, {"positions", array(integer())},
         {"next_sequence", integer()}, {"lag", integer()}, {"in_flight", integer()},
         {"redeliveries", integer()}, {"connected", flag()}, {"group", text()}},
        {"id", "subscriber", "positions", "next_sequence", "lag", "in_flight", "redeliveries",
         "connected", "group"});
    return object({{"topic", text()}, {"settings", settings_schema()},
                   {"subscriber_count", integer()}, {"subscriptions", array(subscription)},
                   {"held_events", integer()}, {"earliest_sequence", integer()},
                   {"next_sequence", integer()}, {"system", flag()}, {"can_manage", flag()},
                   {"counts_available", flag()}, {"zetta", choice({"workspace", "bus"})}},
                  {"topic", "settings", "subscriber_count", "subscriptions", "held_events",
                   "earliest_sequence", "next_sequence", "system", "can_manage",
                   "counts_available", "zetta"});
}

json browse_schema() {
    const auto event = object(
        {{"sequence", integer(1)}, {"id", text()}, {"from", text()}, {"published_at", integer()},
         {"size", integer()}, {"revision", integer(1)}, {"payload", text()}, {"encoding", text()},
         {"headers", array(object({{"name", text()}, {"value", text()}}, {"name", "value"}))},
         {"edited_at", optional_number()}, {"edited_by", text()}},
        {"sequence", "id", "from", "published_at", "size", "revision", "payload", "encoding",
         "headers", "edited_at", "edited_by"});
    return object({{"topic", text()}, {"events", array(event)}, {"earliest_sequence", integer(1)},
                   {"next_sequence", integer(1)}, {"next_page_sequence", optional_number(1)}},
                  {"topic", "events", "earliest_sequence", "next_sequence", "next_page_sequence"});
}

// Registry boundary validation handles shapes; enforce the numeric and enum bounds as well.
bool constraints(const json& schema, const json& value) {
    if (value.is_null()) {
        const auto type = schema.value("type", json());
        return type == "null" || (type.is_array() &&
            std::ranges::find(type, json("null")) != type.end());
    }
    if (schema.contains("enum") && std::ranges::find(schema["enum"], value) == schema["enum"].end()) {
        return false;
    }
    if (value.is_number()) {
        if ((schema.contains("minimum") && value < schema["minimum"]) ||
            (schema.contains("maximum") && value > schema["maximum"])) {
            return false;
        }
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (!constraints(schema.at("properties").at(key), child)) {
                return false;
            }
        }
    }
    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < schema["minItems"].get<std::size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema["maxItems"].get<std::size_t>()) ||
            (schema.value("uniqueItems", false) &&
             std::set<json>(value.begin(), value.end()).size() != value.size())) {
            return false;
        }
        for (const auto& child : value) {
            if (!constraints(schema.at("items"), child)) {
                return false;
            }
        }
    }
    return true;
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(ApiError{"store_unavailable", "no control store", {}});
    }
    context.audit_customer_id = input.at("customer_id");
    context.audit_object = "streams/" + context.audit_customer_id;
    const auto found = context.store->find_policy_object("customer", context.audit_customer_id);
    if (!found) {
        return std::unexpected(ApiError{found.error().code, found.error().message, "/customer_id"});
    }
    return found->id;
}

std::string topic_name(std::string name) {
    if (name.starts_with("topic:")) {
        name.erase(0, 6);
    }
    return name;
}
bool reserved(const std::string& name) {
    return name == "agent-events" || name.starts_with("agent-") ||
           name.starts_with("app-events.") || name.starts_with("build-events.") ||
           name.starts_with("run-metrics.") || name.starts_with("study-events.") ||
           name.starts_with("inbox:");
}

Task proxy(ApiContext& context, const json& input, std::string capability, json args) {
    control::ZettaControlResult<json> result = std::unexpected(
        control::ZettaControlError{"store_unavailable", "the requested Zetta is not configured"});
    if (input.at("zetta") == "workspace" && context.workspaces) {
        result = control::settled(co_await context.workspaces->call(
            input.at("customer_id"), input.at("workspace_id"), std::move(capability), false,
            std::move(args)));
    } else if (input.at("zetta") == "bus" && context.agent_bus) {
        result = control::settled(
            co_await context.agent_bus->bus().call(std::move(capability), std::move(args)));
    }
    if (!result) {
        co_return ApiResult::failure(result.error().code, result.error().message);
    }
    co_return ApiResult::success(std::move(*result));
}

ApiResult completed(std::expected<ApiResult, liquid::error> result) {
    return result ? std::move(*result) : ApiResult::failure("store_error", result.error().message);
}

struct AgentScope {
    std::map<std::string, control::AgentTaskStreamScope> tasks;
    std::set<std::string> agents;
};

std::expected<AgentScope, ApiResult>
agent_scope(ApiContext& context, const std::string& customer) {
    const auto listed = context.store->list_agent_task_stream_scopes(customer);
    if (!listed) {
        return std::unexpected(ApiResult::failure(listed.error().code, listed.error().message));
    }
    AgentScope scope;
    for (const control::AgentTaskStreamScope& task : *listed) {
        scope.agents.insert(task.agent);
        scope.tasks.emplace(task.id, task);
    }
    return scope;
}

bool shared_agent_topic(const std::string& name) {
    return name.starts_with("agent-tasks.");
}

// Only identifiers resolved from this customer's durable rows may address the shared bus.
std::expected<std::string, ApiResult> bus_topic(const json& input, ApiContext& context) {
    const std::string name = topic_name(input.at("topic"));
    const std::string customer = input.at("customer_id");
    if (!name.starts_with("agent-tasks.")) {
        return std::unexpected(ApiResult::failure("not_found", "only agent task dispatch is on the bus"));
    }
    const auto scope = agent_scope(context, customer);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    if (scope->agents.contains(name.substr(12))) {
        return name;
    }
    return std::unexpected(ApiResult::failure("not_found", "no such stream in this space"));
}

Task browse(ApiContext& context, const json& input) {
    const std::string requested = topic_name(input.at("topic"));
    const std::string selected_task = input.value("task", "");
    const std::string selected_agent = input.value("agent", "");
    if (input.at("zetta") == "workspace" && requested == "agent-events" && !selected_task.empty()) {
        const auto task = context.store->get_agent_task(selected_task);
        if (!task || task->customer_id != input.at("customer_id").get<std::string>() ||
            task->workspace_id != input.at("workspace_id").get<std::string>()) {
            co_return ApiResult::failure("not_found", "no such task in this workspace");
        }
    }
    json args = {{"topic", requested},
                 {"direction", input.value("direction", "backward")},
                 {"limit", input.value("limit", 100)}, {"wait_ms", input.value("wait_ms", 0)}};
    if (input.contains("from_sequence")) {
        args["from_sequence"] = input.at("from_sequence");
    }
    bool shared = false;
    std::optional<AgentScope> scope;
    if (input.at("zetta") == "bus") {
        const auto resolved = bus_topic(input, context);
        if (!resolved) {
            co_return resolved.error();
        }
        shared = shared_agent_topic(*resolved);
        args["topic"] = *resolved;
        if (shared) {
            auto selected = agent_scope(context, input.at("customer_id"));
            if (!selected) {
                co_return selected.error();
            }
            scope = std::move(*selected);
            const std::string task = input.value("task", "");
            const std::string agent = input.value("agent", "");
            if ((!task.empty() && !scope->tasks.contains(task)) ||
                (!agent.empty() && !scope->agents.contains(agent)) ||
                (!task.empty() && !agent.empty() && scope->tasks.at(task).agent != agent)) {
                co_return ApiResult::failure("not_found", "no such agent turn in this space");
            }
        }
    }
    json events = json::array();
    json value;
    // Filter before returning anything. The scan remains bounded even on a busy shared bus.
    for (int pages = 0; pages < (shared ? 32 : 1); ++pages) {
        auto result = completed(co_await proxy(context, input, "browse_topic", args));
        if (!result.ok) {
            co_return result;
        }
        value = std::move(result.value);
        const auto& batch = value.at("events");
        value["next_page_sequence"] = nullptr;
        if (!batch.empty()) {
            const auto last = batch.back().at("sequence").get<std::uint64_t>();
            if (args["direction"] == "backward" && last > value.at("earliest_sequence")) {
                value["next_page_sequence"] = last - 1;
            } else if (args["direction"] == "forward" && last + 1 < value.at("next_sequence")) {
                value["next_page_sequence"] = last + 1;
            }
        }
        if (!shared) {
            value["topic"] = requested;
            if (requested == "agent-events" && (!selected_task.empty() || !selected_agent.empty())) {
                auto& rows = value["events"].get_ref<json::array_t&>();
                std::erase_if(rows, [&](const json& event) {
                    const auto payload = json::parse(event.at("payload").get<std::string>(), nullptr, false);
                    return !payload.is_object() ||
                        (!selected_task.empty() && payload.value("task_id", "") != selected_task) ||
                        (!selected_agent.empty() && payload.value("agent", "") != selected_agent);
                });
            }
            co_return ApiResult::success(std::move(value));
        }
        for (const auto& event : batch) {
            const auto payload = json::parse(event.at("payload").get<std::string>(), nullptr, false);
            if (!payload.is_object()) {
                continue;
            }
            const std::string task_id = payload.value("task_id", "");
            const auto task = scope->tasks.find(task_id);
            const bool selected_task = task != scope->tasks.end() &&
                                       (input.value("task", "").empty() ||
                                        input.value("task", "") == task_id) &&
                                       (input.value("agent", "").empty() ||
                                        input.value("agent", "") == task->second.agent);
            const bool selected_queue = requested.starts_with("agent-tasks.") &&
                                        payload.value("customer_id", "") ==
                                            input.at("customer_id").get<std::string>();
            if ((requested == "agent-events" && selected_task) || selected_queue) {
                events.push_back(event);
            }
        }
        if (!events.empty() || batch.empty()) {
            break;
        }
        const auto last = batch.back().at("sequence").get<std::uint64_t>();
        if (args["direction"] == "backward" && last == 1) {
            break;
        }
        args["from_sequence"] = args["direction"] == "backward" ? last - 1 : last + 1;
        args["wait_ms"] = 0;
    }
    value["events"] = std::move(events);
    value["topic"] = requested;
    // The allocator mark is needed to continue a bounded forward scan, including empty pages.
    co_return ApiResult::success(std::move(value));
}

liquid::runtime::task<std::expected<control::StreamOwner, ApiResult>>
owner_key(ApiContext& context, const json& input) {
    if (!context.workspaces) {
        co_return std::expected<control::StreamOwner, ApiResult>(std::unexpect,
            ApiResult::failure("store_unavailable", "workspace unavailable"));
    }
    const auto state = control::settled(
        co_await context.workspaces->state(input.at("customer_id"), input.at("workspace_id")));
    if (!state || !state->ready()) {
        co_return std::expected<control::StreamOwner, ApiResult>(std::unexpect,
            ApiResult::failure("store_unavailable", "workspace is not ready"));
    }
    co_return control::StreamOwner{input.at("customer_id"), input.at("workspace_id"),
                                  topic_name(input.at("topic")), context.acting_identity};
}

Task get(ApiContext& context, const json& input) {
    const std::string requested = topic_name(input.at("topic"));
    std::string name = requested;
    const bool bus = input.at("zetta") == "bus";
    if (bus) {
        auto resolved = bus_topic(input, context);
        if (!resolved) {
            co_return resolved.error();
        }
        name = *resolved;
    }
    auto result = completed(co_await proxy(context, input, "get_topic", {{"topic", name}}));
    if (!result.ok) {
        co_return result;
    }
    result.value["topic"] = requested;
    result.value["zetta"] = input.at("zetta");
    result.value["system"] = bus || reserved(name);
    result.value["can_manage"] = false;
    result.value["counts_available"] = true;
    if (!bus && !reserved(name)) {
        auto key = co_await owner_key(context, input);
        if (key && *key) {
            auto creator = context.store->stream_owner(**key);
            if (!creator) {
                co_return ApiResult::failure(creator.error().code, creator.error().message);
            }
            result.value["can_manage"] = *creator == context.acting_identity;
        }
    }
    if (bus && shared_agent_topic(name)) {
        // Shared subscription identities and aggregate counts never leave the service.
        result.value["subscriptions"] = json::array();
        result.value["subscriber_count"] = 0;
        result.value["held_events"] = 0;
        result.value["earliest_sequence"] = result.value.at("next_sequence");
        result.value["counts_available"] = false;
    }
    co_return result;
}

std::string stream_name(const std::string& topic) {
    if (topic == "agent-events") {
        return "Agent events";
    }
    if (topic.starts_with("agent-tasks.")) {
        return "Agent tasks · " + topic.substr(12);
    }
    if (topic.starts_with("agent-turn.")) {
        return "Agent turn · " + topic.substr(11);
    }
    return topic;
}

Task list(ApiContext& context, const json& input) {
    const auto object = customer_object(input, context);
    if (!object) {
        co_return ApiResult::failure(object.error().code, object.error().message,
                                     object.error().path);
    }
    if (!context.operator_credential) {
        const auto decision = control::authorize(*context.store, context.acting_identity,
                                                 *object, "read", context.authorization_cache);
        if (!decision) {
            co_return ApiResult::failure(decision.error().code, decision.error().message);
        }
        if (!decision->allowed) {
            co_return ApiResult::failure("unauthorized", "principal cannot read this space's streams");
        }
    }
    const std::string customer = input.at("customer_id");
    const std::string prefix = input.value("prefix", "");
    const auto agents = agent_scope(context, customer);
    if (!agents) {
        co_return agents.error();
    }
    json unpaged = json::array();
    for (const std::string zetta : {"workspace", "bus"}) {
        json target = input;
        target["zetta"] = zetta;
        auto result = completed(co_await proxy(context, target, "list_topics", json::object()));
        json rows = json::array();
        std::string error;
        if (!result.ok) {
            error = result.error.message;
        } else {
            for (auto row : result.value.at("topics")) {
                const auto name = topic_name(row.at("topic"));
                if (!name.starts_with(prefix)) {
                    continue;
                }
                bool allowed = zetta == "workspace";
                bool counts_available = true;
                if (zetta == "bus") {
                    allowed = false;
                    if (name.starts_with("agent-tasks.")) {
                        allowed = agents->agents.contains(name.substr(12));
                    }
                    counts_available = !shared_agent_topic(name);
                }
                if (!allowed) {
                    continue;
                }
                row["topic"] = name;
                row["display_name"] = stream_name(name);
                row["system"] = zetta == "bus" || reserved(name);
                row["counts_available"] = counts_available;
                row["subscribers"] = counts_available ? row.value("subscribers", 0) : 0;
                row["held_events"] = counts_available ? row.value("held_events", 0) : 0;
                rows.push_back(std::move(row));
            }
            auto& topic_rows = rows.get_ref<json::array_t&>();
            std::ranges::sort(topic_rows, {}, [](const json& row) {
                return row.at("topic").get<std::string>();
            });
        }
        unpaged.push_back({{"zetta", zetta}, {"topics", std::move(rows)}, {"error", error}});
    }

    std::size_t total = 0;
    for (const json& group : unpaged) {
        total += group.at("topics").size();
    }
    std::size_t offset = 0;
    if (input.contains("cursor")) {
        const std::string cursor = input.at("cursor");
        const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
        if (cursor.empty() || parsed.ec != std::errc() || parsed.ptr != cursor.data() + cursor.size() ||
            offset > total) {
            co_return ApiResult::failure("invalid_input", "stream cursor is invalid", "/cursor");
        }
    }
    const std::size_t limit = input.value("limit", std::size_t{50});
    const std::size_t end = std::min(offset + limit, total);
    std::size_t position = 0;
    json groups = json::array();
    for (json& group : unpaged) {
        json rows = json::array();
        for (json& row : group.at("topics")) {
            if (position >= offset && position < end) {
                rows.push_back(std::move(row));
            }
            ++position;
        }
        groups.push_back({{"zetta", group.at("zetta")}, {"topics", std::move(rows)},
                          {"error", group.at("error")}});
    }
    json next = end < total ? json(std::to_string(end)) : json(nullptr);
    json previous = offset > 0 ? json(std::to_string(offset > limit ? offset - limit : 0))
                               : json(nullptr);
    co_return ApiResult::success({{"groups", std::move(groups)}, {"next_cursor", std::move(next)},
                                  {"previous_cursor", std::move(previous)}, {"total", total}});
}

Task count_changes(ApiContext& context, const json& input, const json& settings, bool retention) {
    json args = input;
    args["direction"] = "backward";
    args["limit"] = 1000;
    args["wait_ms"] = 0;
    std::uint64_t seen = 0;
    std::uint64_t count = 0;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    while (true) {
        const auto page = completed(co_await browse(context, args));
        if (!page.ok) {
            co_return page;
        }
        const auto& events = page.value.at("events");
        for (const auto& event : events) {
            ++seen;
            const auto sequence = event.at("sequence").get<std::uint64_t>();
            bool selected = false;
            if (retention) {
                selected = settings.contains("max_events") && !settings["max_events"].is_null() &&
                           seen > settings["max_events"].get<std::uint64_t>();
                selected = selected || (settings.contains("retention_ms") &&
                    !settings["retention_ms"].is_null() && now - event.at("published_at").get<std::int64_t>() >=
                    settings["retention_ms"].get<std::int64_t>());
            } else if (input.contains("before_sequence")) {
                selected = sequence < input["before_sequence"].get<std::uint64_t>();
            } else {
                selected = std::ranges::find(input.at("sequences"), json(sequence)) !=
                           input.at("sequences").end();
            }
            count += selected;
        }
        if (events.empty() || events.back().at("sequence") == 1) {
            break;
        }
        args["from_sequence"] = events.back().at("sequence").get<std::uint64_t>() - 1;
    }
    co_return ApiResult::success({{"count", count}});
}

Task mutate(ApiContext& context, const json& input, const std::string& name) {
    const std::string topic = topic_name(input.at("topic"));
    if (input.at("zetta") != "workspace" || reserved(topic)) {
        co_return ApiResult::failure("unauthorized", "system streams are browse-only");
    }
    auto pending = co_await owner_key(context, input);
    if (!pending) {
        co_return ApiResult::failure("store_error", pending.error().message);
    }
    if (!*pending) {
        co_return pending->error();
    }
    auto key = **pending;
    const auto creator = context.store->stream_owner(key);
    if (!creator) {
        co_return ApiResult::failure(creator.error().code, creator.error().message);
    }
    const bool create = name == "create_stream";
    if (create && !creator->empty()) {
        co_return ApiResult::failure("conflict", "a recorded topic creation already exists");
    }
    if ((name == "update_stream" || name == "delete_stream") &&
        *creator != context.acting_identity) {
        co_return ApiResult::failure("unauthorized", "only the topic creator may change this stream");
    }
    json args = input;
    for (const auto* field : {"customer_id", "workspace_id", "zetta", "dry_run", "confirm_name", "confirm_count"}) {
        args.erase(field);
    }
    args["topic"] = topic;
    std::string upstream;
    if (create) {
        args["settings"] = input.value("settings", json::object());
        if (args["settings"].contains("exposed") && args["settings"]["exposed"] != true) {
            co_return ApiResult::failure("invalid_input", "Streams topics must be exposed");
        }
        args["settings"]["exposed"] = true;
        if (!args["settings"].contains("max_events")) {
            args["settings"]["max_events"] = 1000;
        }
        upstream = "create_topic";
    } else if (name == "update_stream") {
        if (input.value("dry_run", false)) {
            const auto current = completed(co_await get(context, input));
            if (!current.ok) {
                co_return current;
            }
            auto settings = current.value.at("settings");
            settings.merge_patch(input.at("settings"));
            const auto counted = completed(co_await count_changes(context, input, settings, true));
            if (!counted.ok) {
                co_return counted;
            }
            co_return ApiResult::success({{"topic", "topic:" + topic},
                {"settings", current.value.at("settings")}, {"trimmed", counted.value.at("count")},
                {"dry_run", true}});
        }
        upstream = "update_topic";
    } else if (name == "delete_stream") {
        if (topic_name(input.value("confirm_name", "")) != topic) {
            co_return ApiResult::failure("invalid_input", "type the topic name to confirm deletion");
        }
        upstream = "drop_topic";
    } else if (name == "publish_stream_message" || name == "update_stream_message") {
        if (json::parse(input.at("payload").get<std::string>(), nullptr, false).is_discarded()) {
            co_return ApiResult::failure("invalid_input", "payload must contain valid JSON", "/payload");
        }
        upstream = name == "publish_stream_message" ? "publish_event" : "update_event";
        if (upstream == "publish_event") {
            args["encoding"] = "json";
        }
    } else {
        if (input.contains("sequences") == input.contains("before_sequence") ||
            (input.contains("sequences") && (input["sequences"].empty() ||
             input["sequences"].size() > 1000 ||
             std::set<json>(input["sequences"].begin(), input["sequences"].end()).size() !=
             input["sequences"].size()))) {
            co_return ApiResult::failure("invalid_input", "choose unique sequences or before_sequence");
        }
        const auto counted = completed(co_await count_changes(context, input, json::object(), false));
        if (!counted.ok) {
            co_return counted;
        }
        if (input.value("dry_run", false)) {
            co_return ApiResult::success({{"topic", "topic:" + topic},
                {"removed", counted.value.at("count")}, {"dry_run", true}});
        }
        if (!input.contains("confirm_count") || input["confirm_count"] != counted.value.at("count")) {
            co_return ApiResult::failure("conflict", "message count changed; preview and confirm again");
        }
        upstream = "delete_events";
    }
    const auto result = completed(co_await proxy(context, input, upstream, args));
    if (create && result.ok) {
        // Only a confirmed upstream creation establishes ownership. A lost response or an
        // existing topic must never grant control of a topic this person did not create.
        const auto claimed = context.store->create_stream_owner(key);
        if (!claimed) {
            co_return ApiResult::failure(claimed.error().code, claimed.error().message);
        }
    }
    if (name == "delete_stream" && result.ok) {
        const auto released = context.store->delete_stream_owner(key);
        if (!released) {
            co_return ApiResult::failure(released.error().code, released.error().message);
        }
    }
    co_return result;
}

void add(Registry& registry, std::string name, json properties, json required, json output,
         std::string permission) {
    properties["customer_id"] = text();
    required.push_back("customer_id");
    properties["workspace_id"] = text();
    required.push_back("workspace_id");
    if (name != "list_streams") {
        properties["zetta"] = choice({"workspace", "bus"});
        properties["topic"] = text();
        required.push_back("zetta");
        required.push_back("topic");
    }
    auto schema = object(std::move(properties), std::move(required));
    if (name == "delete_stream_messages") {
        schema["oneOf"] = {{{"required", {"sequences"}}}, {{"required", {"before_sequence"}}}};
    }
    const bool read = permission == "read";
    registry.add(Capability{
        .name = name, .title = name,
        .description = read ? "Read space streams without consuming messages."
                            : "Change a person-created stream after the person confirms.",
        .input_schema = schema, .output_schema = std::move(output),
        .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
        .post_alias = read, .object = "customer", .action = std::move(permission),
        .resolve_object = customer_object, .authorizes_candidates = name == "list_streams",
        .resolves_operator_audit_target = true,
        .async_handler = [name, schema](const json& input, ApiContext& context) -> Task {
            if (!constraints(schema, input)) {
                co_return ApiResult::failure("invalid_input", "stream argument is outside its bounds");
            }
            if (input.contains("topic")) {
                const auto topic = topic_name(input.at("topic"));
                if (topic.empty() || topic.size() > 512 || topic.find_first_of("*? /\\\t\r\n") != std::string::npos ||
                    (topic.contains(':') && !topic.starts_with("inbox:"))) {
                    co_return ApiResult::failure("invalid_input", "a concrete topic name is required");
                }
            }
            context.audit_kind = "stream." + name;
            context.audit_details = {{"customer_id", input.at("customer_id")},
                {"zetta", input.value("zetta", "both")}, {"topic", input.value("topic", "")}};
            for (const auto* field : {"sequence", "expected_revision", "redeliver", "sequences",
                                      "before_sequence", "confirm_count", "dry_run", "settings",
                                      "prefix", "limit", "cursor", "task", "agent"}) {
                if (input.contains(field)) {
                    context.audit_details[field] = input.at(field);
                }
            }
            if (name == "list_streams") {
                co_return completed(co_await list(context, input));
            }
            if (name == "get_stream") {
                co_return completed(co_await get(context, input));
            }
            if (name == "browse_stream") {
                co_return completed(co_await browse(context, input));
            }
            co_return completed(co_await mutate(context, input, name));
        }});
}
} // namespace

void register_stream_capabilities(Registry& registry) {
    const auto summary = object({{"topic", text()}, {"display_name", text()},
        {"subscribers", integer()}, {"held_events", integer()}, {"next_sequence", integer(1)},
        {"system", flag()}, {"counts_available", flag()}},
        {"topic", "display_name", "subscribers", "held_events", "next_sequence", "system",
         "counts_available"});
    add(registry, "list_streams", {{"prefix", text()},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
        {"cursor", text()}}, json::array(),
        object({{"groups", array(object({{"zetta", choice({"workspace", "bus"})},
            {"topics", array(summary)}, {"error", text()}}, {"zetta", "topics", "error"}))},
            {"next_cursor", optional_text()}, {"previous_cursor", optional_text()},
            {"total", integer()}}, {"groups", "next_cursor", "previous_cursor", "total"}), "read");
    add(registry, "get_stream", json::object(), json::array(), topic_schema(), "read");
    add(registry, "browse_stream", {{"from_sequence", integer(1)},
        {"direction", choice({"forward", "backward"})},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
        {"task", text()}, {"agent", text()}},
        json::array(), browse_schema(), "read");
    add(registry, "create_stream", {{"settings", settings_schema()}}, json::array(),
        object({{"topic", text()}, {"exposed", flag()}, {"settings", settings_schema()}},
            {"topic", "exposed", "settings"}), "write");
    add(registry, "update_stream", {{"settings", settings_schema(true)}, {"dry_run", flag()}},
        {"settings"}, object({{"topic", text()}, {"settings", settings_schema()},
            {"trimmed", integer()}, {"dry_run", flag()}}, {"topic", "settings", "trimmed"}), "write");
    add(registry, "delete_stream", {{"confirm_name", text()}}, {"confirm_name"},
        object({{"topic", text()}, {"exposed", flag()}}, {"topic", "exposed"}), "admin");
    add(registry, "publish_stream_message", {{"payload", text()}}, {"payload"},
        object({{"event_id", text()}, {"accepted", flag()}, {"clamped", array(text())}},
            {"event_id", "accepted", "clamped"}), "write");
    add(registry, "update_stream_message", {{"payload", text()}, {"sequence", integer(1)},
        {"expected_revision", integer(1)}, {"redeliver", flag()}},
        {"payload", "sequence", "expected_revision"},
        object({{"topic", text()}, {"sequence", integer(1)}, {"revision", integer(1)},
            {"edited_by", text()}, {"edited_at", integer()}},
            {"topic", "sequence", "revision", "edited_by", "edited_at"}), "write");
    auto sequences = array(integer(1));
    sequences.update({{"minItems", 1}, {"maxItems", 1000}, {"uniqueItems", true}});
    add(registry, "delete_stream_messages", {{"sequences", std::move(sequences)},
        {"before_sequence", integer(1)}, {"dry_run", flag()}, {"confirm_count", integer()}},
        json::array(), object({{"topic", text()}, {"removed", integer()}, {"dry_run", flag()}},
            {"topic", "removed"}), "admin");
}
} // namespace lake::api
