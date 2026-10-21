// An agent started by a workspace stream (lake#677, docs/api/agent-launches.md#stream-triggers).
// An agent's stream triggers name topics of its workspace Zetta; each new message on one starts a
// job of the agent with the message as its input, as the person who set the trigger, the way a
// schedule's runs act as whoever set the schedule. The triggers are sealed records in the
// workspace Zetta (`lake_agent_stream_triggers`); lake's Postgres holds only the launches they
// start, as it holds every launch. The store root's owner reads the streams, and only it, with the
// machinery the MCP connection agents use (agent_stream_runs.hpp, lake#652).

#include "lake/api/agent_stream_triggers.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

#include "agent_stream_runs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/control/agent_stream_triggers.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "workspace_bus.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
using control::AgentStreamTrigger;

constexpr std::size_t max_streams_per_agent = 20;
constexpr std::size_t max_events_per_trigger = 20;
constexpr std::size_t max_triggers_per_pass = 256;

// Streams Lake and its agents write themselves: never a trigger, so an agent's own events can
// never start it again (the same names Train → Streams shows as system streams).
bool system_stream(const std::string& name) {
    return name.starts_with("agent-") || name.starts_with("app-events.") ||
           name.starts_with("build-events.") || name.starts_with("run-metrics.") ||
           name.starts_with("study-events.") || name.starts_with("inbox:");
}

std::string stream_of(std::string name) {
    if (name.starts_with("topic:")) {
        name.erase(0, 6);
    }
    return name;
}

bool stream_name_ok(const std::string& name) {
    return !name.empty() && name.size() <= 512 &&
           name.find_first_of("*? /\\\t\r\n:") == std::string::npos;
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

json trigger_json(const ApiContext& context, const AgentStreamTrigger& trigger) {
    json out{{"id", trigger.id},
             {"agent_id", trigger.agent_id},
             {"agent_name", trigger.agent_name},
             {"stream", trigger.topic},
             {"zetta", "workspace"},
             {"cursor", trigger.cursor},
             {"set_by", trigger.set_by},
             {"set_by_name", nullptr},
             {"created_at", trigger.created_at}};
    if (context.store != nullptr && !trigger.set_by.empty()) {
        if (const auto user = context.store->get_user(trigger.set_by)) {
            out["set_by_name"] = user->display_name.empty() ? user->email : user->display_name;
        }
    }
    return out;
}

json workspace_schema(json properties, json required) {
    properties["customer_id"] = {{"type", "string"}};
    properties["workspace_id"] = {{"type", "string"}};
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json streams_output_schema() {
    return {{"type", "object"},
            {"properties",
             {{"agent_id", {{"type", "string"}}},
              {"streams", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
}

liquid::runtime::task<ApiResult> set_agent_streams(const json& input, ApiContext& context) {
    if (!context.workspaces || !context.store) {
        co_return ApiResult::failure("store_unavailable",
                                     "workspaces are not configured on this Lake");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string agent_id = input.at("agent_id");
    context.workspace_id = workspace_id;
    std::vector<std::string> wanted;
    const json& streams = input.at("streams");
    for (std::size_t index = 0; index < streams.size(); ++index) {
        const std::string path = "/streams/" + std::to_string(index);
        const std::string name = stream_of(streams[index].get<std::string>());
        if (!stream_name_ok(name)) {
            co_return ApiResult::failure(
                "invalid_input", "a stream is named by one of the workspace's topics", path);
        }
        if (system_stream(name)) {
            co_return ApiResult::failure("invalid_input",
                                         name +
                                             " is a system stream Lake writes; choose one of the "
                                             "workspace's own streams",
                                         path);
        }
        if (std::ranges::find(wanted, name) == wanted.end()) {
            wanted.push_back(name);
        }
    }
    if (wanted.size() > max_streams_per_agent) {
        co_return ApiResult::failure("invalid_input",
                                     "an agent starts from at most " +
                                         std::to_string(max_streams_per_agent) + " streams",
                                     "/streams");
    }
    // A run acts as the person who set the trigger, so a trigger needs a person.
    if (!wanted.empty() && context.acting_user_id.empty()) {
        co_return ApiResult::failure("invalid_input",
                                     "a stream's runs act as the person who sets it; set it "
                                     "signed in as a person");
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto existing = control::settled(
        co_await control::list_agent_stream_triggers(*records, customer_id, agent_id));
    if (!existing) {
        co_return failed(existing.error());
    }
    std::string agent_name;
    if (!wanted.empty()) {
        auto agent = co_await call_workspace(
            context, customer_id, "get_agent", workspace_uses_get("get_agent"), {{"id", agent_id}});
        if (!agent) {
            co_return ApiResult::failure("store_unavailable", agent.error().message);
        }
        if (!*agent) {
            if (agent->error().code == "not_found") {
                co_return ApiResult::failure(
                    "not_found", "no such agent in this workspace", "/agent_id");
            }
            co_return failed(agent->error());
        }
        if ((*agent)->value("runtime", std::string("native")) != "python") {
            co_return ApiResult::failure("invalid_input",
                                         "only a frontier-model (Python) agent runs from a "
                                         "stream, as only one runs on a schedule",
                                         "/agent_id");
        }
        agent_name = (*agent)->value("name", std::string());
    }

    // Every new stream must exist; a new trigger starts after the messages already on it.
    std::vector<AgentStreamTrigger> added;
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        const std::string& name = wanted[index];
        if (std::ranges::any_of(*existing,
                                [&](const AgentStreamTrigger& t) { return t.topic == name; })) {
            continue;
        }
        auto topic =
            co_await call_workspace(context, customer_id, "get_topic", false, {{"topic", name}});
        if (!topic) {
            co_return ApiResult::failure("store_unavailable", topic.error().message);
        }
        if (!*topic) {
            if (topic->error().code == "not_found") {
                co_return ApiResult::failure("not_found",
                                             "this workspace has no stream named " + name,
                                             "/streams/" + std::to_string(index));
            }
            co_return failed(topic->error());
        }
        const std::uint64_t next = (*topic)->value("next_sequence", std::uint64_t{1});
        added.push_back({.customer_id = customer_id,
                         .agent_id = agent_id,
                         .agent_name = agent_name,
                         .topic = name,
                         .cursor = next > 0 ? next - 1 : 0,
                         .set_by = context.acting_user_id,
                         .set_by_identity = context.acting_identity});
    }

    json removed_streams = json::array();
    for (const AgentStreamTrigger& trigger : *existing) {
        if (std::ranges::find(wanted, trigger.topic) != wanted.end()) {
            continue;
        }
        auto removed = control::settled(
            co_await control::remove_agent_stream_trigger(*records, customer_id, trigger.id));
        if (!removed) {
            co_return failed(removed.error());
        }
        removed_streams.push_back(trigger.topic);
    }
    json added_streams = json::array();
    for (AgentStreamTrigger& trigger : added) {
        const std::string name = trigger.topic;
        auto stored = control::settled(
            co_await control::record_agent_stream_trigger(*records, std::move(trigger)));
        if (!stored) {
            co_return failed(stored.error());
        }
        added_streams.push_back(name);
    }
    context.audit_kind = "agent.streams";
    context.audit_details = {{"customer_id", customer_id},
                             {"agent_id", agent_id},
                             {"streams", wanted},
                             {"added", added_streams},
                             {"removed", removed_streams}};

    auto now = control::settled(
        co_await control::list_agent_stream_triggers(*records, customer_id, agent_id));
    if (!now) {
        co_return failed(now.error());
    }
    json out = json::array();
    for (const AgentStreamTrigger& trigger : *now) {
        out.push_back(trigger_json(context, trigger));
    }
    co_return ApiResult::success({{"agent_id", agent_id}, {"streams", std::move(out)}});
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context);

liquid::runtime::task<ApiResult> list_agent_streams(const json& input, ApiContext& context) {
    // A list_ read authorizes itself (read on the account), as every list_ read does.
    if (context.acting_identity != "admin") {
        const auto object = customer_object(input, context);
        if (!object) {
            co_return ApiResult::failure(
                object.error().code, object.error().message, object.error().path);
        }
        const auto decision = control::authorize(*context.store,
                                                 context.acting_identity,
                                                 *object,
                                                 "read",
                                                 context.authorization_cache);
        if (!decision) {
            co_return ApiResult::failure(decision.error().code, decision.error().message);
        }
        if (!decision->allowed) {
            co_return ApiResult::failure("unauthorized",
                                         "principal is not allowed to read the customer");
        }
    }
    if (!context.workspaces) {
        co_return ApiResult::failure("store_unavailable",
                                     "workspaces are not configured on this Lake");
    }
    const std::string customer_id = input.at("customer_id");
    context.workspace_id = input.at("workspace_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto listed = control::settled(co_await control::list_agent_stream_triggers(
        *records, customer_id, input.value("agent_id", std::string())));
    if (!listed) {
        co_return failed(listed.error());
    }
    json out = json::array();
    for (const AgentStreamTrigger& trigger : *listed) {
        out.push_back(trigger_json(context, trigger));
    }
    co_return ApiResult::success({{"streams", std::move(out)}});
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

} // namespace

std::optional<ApiContext>
stream_person(const ApiContext& base, const std::string& user_id, std::string method) {
    if (user_id.empty() || base.store == nullptr) {
        return std::nullopt;
    }
    const auto user = base.store->get_user(user_id);
    // The person's identity is a TAAP id, never the operator's (lake#870).
    if (!user || !control::valid_principal_id(user->taap_id)) {
        return std::nullopt;
    }
    ApiContext context = base;
    context.authorized = true;
    context.operator_credential = false;
    context.session_credential = false;
    context.acting_user_id = user->id;
    context.acting_identity = user->taap_id;
    context.authentication_method = std::move(method);
    context.request_id = control::new_uuid_v4();
    return context;
}

StreamRuns start_stream_runs(const ApiContext& context,
                             const ApiContext& person,
                             control::AgentBus& bus,
                             const std::string& customer_id,
                             const std::string& workspace_id,
                             const std::string& agent_id,
                             const std::string& topic,
                             std::uint64_t cursor,
                             std::size_t max_events,
                             const std::function<json(const control::BusEvent&)>& input_of) {
    StreamRuns runs;
    runs.cursor = cursor;
    if (context.registry == nullptr) {
        return runs;
    }
    auto batch = liquid::runtime::block_on(
        bus.receive(topic, cursor + 1, static_cast<int>(max_events), std::chrono::milliseconds(0)));
    if (!batch || !*batch) {
        if (batch && (*batch).error().code == "not_found") {
            runs.unreadable = "the stream " + topic + " does not exist";
        } else {
            runs.unreadable = "the stream " + topic + " could not be read: " +
                              (!batch ? batch.error().message : (*batch).error().message);
        }
        return runs;
    }
    for (const control::BusEvent& event : (*batch)->events) {
        if (event.sequence <= cursor) {
            continue;
        }
        ApiContext as = person;
        auto launched =
            liquid::runtime::block_on(context.registry->call_async("launch_agent",
                                                                   {{"customer_id", customer_id},
                                                                    {"workspace_id", workspace_id},
                                                                    {"agent_id", agent_id},
                                                                    {"mode", "job"},
                                                                    {"input", input_of(event)}},
                                                                   as));
        runs.cursor = std::max(runs.cursor, event.sequence);
        if (launched && launched->ok) {
            const std::string id =
                launched->value.value("id", launched->value.value("launch_id", std::string()));
            if (!id.empty()) {
                runs.launched.push_back(id);
            }
            continue;
        }
        if (launched && launched->error.code == "not_found") {
            runs.agent_gone = true;
            runs.problems.push_back("its agent no longer exists");
            break;
        }
        runs.problems.push_back("its agent could not start: " +
                                (launched ? launched->error.message : launched.error().message));
    }
    return runs;
}

liquid::runtime::task<control::ZettaControlResult<std::size_t>>
remove_agent_stream_triggers(const ApiContext& context,
                             std::string customer_id,
                             std::string workspace_id,
                             std::string agent_id) {
    using Result = control::ZettaControlResult<std::size_t>;
    if (agent_id.empty()) {
        co_return Result(std::size_t{0});
    }
    auto records = workspace_records(context, customer_id, std::move(workspace_id));
    if (!records) {
        co_return Result(std::unexpected(records.error()));
    }
    auto listed = control::settled(
        co_await control::list_agent_stream_triggers(*records, customer_id, agent_id));
    if (!listed) {
        co_return Result(std::unexpected(listed.error()));
    }
    std::size_t removed = 0;
    for (const AgentStreamTrigger& trigger : *listed) {
        auto gone = control::settled(
            co_await control::remove_agent_stream_trigger(*records, customer_id, trigger.id));
        if (!gone) {
            co_return Result(std::unexpected(gone.error()));
        }
        ++removed;
    }
    co_return Result(removed);
}

AgentStreamPass AgentStreamWorker::pass(ApiContext& context) {
    AgentStreamPass report;
    if (context.store == nullptr || context.workspaces == nullptr || context.registry == nullptr) {
        return report;
    }
    const auto customers = context.store->list_customers(100000);
    if (!customers) {
        report.problems.push_back("accounts: " + customers.error().message);
        return report;
    }
    for (const control::Customer& customer : *customers) {
        const auto workspaces = context.store->list_workspaces(customer.id);
        if (!workspaces) {
            continue;
        }
        for (const control::Workspace& workspace : *workspaces) {
            // A workspace that is not running, or whose Zetta is not ready, answers the list below
            // with an error and is passed over.
            if (workspace.desired != "running" || report.triggers >= max_triggers_per_pass) {
                continue;
            }
            ++report.workspaces;
            auto records = workspace_records(context, customer.id, workspace.id);
            if (!records) {
                continue;
            }
            auto listed = liquid::runtime::block_on(
                control::list_agent_stream_triggers(*records, customer.id));
            if (!listed || !*listed) {
                continue;
            }
            WorkspaceApiBus bus(context, customer.id, workspace.id);
            for (const AgentStreamTrigger& trigger : **listed) {
                if (report.triggers >= max_triggers_per_pass) {
                    break;
                }
                ++report.triggers;
                const std::string label =
                    (trigger.agent_name.empty() ? trigger.agent_id : trigger.agent_name) + " on " +
                    trigger.topic;
                const auto person = stream_person(context, trigger.set_by, "agent_stream_trigger");
                if (!person) {
                    report.problems.push_back(label + ": the person who set it is no longer known");
                    continue;
                }
                const StreamRuns runs =
                    start_stream_runs(context,
                                      *person,
                                      bus,
                                      customer.id,
                                      workspace.id,
                                      trigger.agent_id,
                                      trigger.topic,
                                      trigger.cursor,
                                      max_events_per_trigger,
                                      [&](const control::BusEvent& event) {
                                          return json{{"trigger_id", trigger.id},
                                                      {"stream", trigger.topic},
                                                      {"sequence", event.sequence},
                                                      {"message", event.payload}};
                                      });
                report.runs += runs.launched.size();
                if (!runs.unreadable.empty()) {
                    report.problems.push_back(label + ": " + runs.unreadable);
                }
                for (const std::string& problem : runs.problems) {
                    report.problems.push_back(label + ": " + problem);
                }
                if (runs.agent_gone) {
                    // An agent deleted outside Lake's delete_agent still loses its triggers.
                    auto gone = liquid::runtime::block_on(
                        control::remove_agent_stream_trigger(*records, customer.id, trigger.id));
                    if (gone && *gone) {
                        ++report.removed;
                    }
                    continue;
                }
                if (runs.cursor != trigger.cursor) {
                    auto moved = liquid::runtime::block_on(control::advance_agent_stream_trigger(
                        *records, customer.id, trigger.id, runs.cursor));
                    if (!moved || !*moved) {
                        report.problems.push_back(label + ": its place on the stream was not kept");
                    }
                }
            }
        }
    }
    return report;
}

void register_agent_stream_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "set_agent_streams",
        .title = "Set the streams that start an agent",
        .description =
            "Name the workspace streams (list_streams) whose new messages each start a run of a "
            "Python agent: one job per message, with {trigger_id, stream, sequence, message} as "
            "its input, acting as the person who set the stream, as a schedule's runs act as "
            "whoever set the schedule. `streams` is the whole set: a stream left out stops "
            "starting the agent, an empty list removes them all. A new stream starts with the "
            "messages that arrive after it is set. System streams (agent-*, app-events.*, "
            "build-events.*, run-metrics.*, study-events.*) are refused. Deleting the agent "
            "removes its streams.",
        .input_schema = workspace_schema({{"agent_id", {{"type", "string"}, {"minLength", 1}}},
                                          {"streams",
                                           {{"type", "array"},
                                            {"items", {{"type", "string"}, {"minLength", 1}}},
                                            {"maxItems", max_streams_per_agent}}}},
                                         json::array({"agent_id", "streams"})),
        .output_schema = streams_output_schema(),
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = set_agent_streams});
    registry.add(Capability{
        .name = "list_agent_streams",
        .title = "List the streams that start agents",
        .description =
            "The workspace streams that start the workspace's agents (or one agent's), oldest "
            "first: each with its stream, the agent, who set it (the person its runs act as), "
            "when, "
            "and the last message sequence that started a run.",
        .input_schema = workspace_schema({{"agent_id", {{"type", "string"}}}}, json::array()),
        .output_schema = streams_output_schema(),
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = list_agent_streams});
}

} // namespace lake::api
