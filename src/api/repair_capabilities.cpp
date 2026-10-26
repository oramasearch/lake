// When a resource fails (lake#1207, docs/api/notifications.md): Lake tells the person who deployed
// it, offers an agent to fix it (a `repair` turn) or to change it from instructions (a `change`
// turn), and, when the person opts in, lets the agent repair it itself within a bounded number of
// attempts. The mechanism is general over the resource kind: notifications and repair records name
// a kind and an id (control/resource_repairs.hpp). Apps (kind `app`, repaired by the Application
// Agent) are implemented here; custom agents (kind `agent`, repaired by agent-creator, lake#1209)
// add a detector to the pass, a repairing agent below and their own repair/change capabilities.
// The turns go through agent_turn, so they carry the person's own permissions, the balance gate,
// audit and metering.

#include "lake/api/resource_repairs.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "agent_stream_runs.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/resource_repairs.hpp"
#include "lake/control/store.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

// The agent that repairs and changes each kind of resource, and the context key naming it.
struct Repairer {
    std::string_view agent;
    std::string_view context_key;
};

std::optional<Repairer> repairer_of(std::string_view kind) {
    if (kind == control::resource_kind_app) {
        return Repairer{"agent-application", "app_id"};
    }
    return std::nullopt;
}

json input_schema(json properties, json required = json::array()) {
    properties["customer_id"] = {{"type", "string"}};
    properties["workspace_id"] = {{"type", "string"}};
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json resource_kind_schema() {
    return {{"type", "string"}, {"enum", json::array({std::string(control::resource_kind_app)})}};
}

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const std::string customer_id = input.at("customer_id");
    context.audit_customer_id = customer_id;
    context.audit_object = "customer/" + customer_id;
    const auto object = context.store->find_policy_object("customer", customer_id);
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

// A live resource of this workspace: its display name, or why not.
struct Resource {
    std::string kind;
    std::string id;
    std::string name;
    std::string customer_id;
    std::string workspace_id;
};

std::expected<Resource, ApiResult> owned_resource(const std::string& kind,
                                                  const std::string& id,
                                                  const json& input,
                                                  ApiContext& context,
                                                  std::string path) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    if (kind == control::resource_kind_app) {
        auto app = context.store->get_web_app(id);
        if (app && app->customer_id == customer_id && app->workspace_id == workspace_id &&
            app->desired != "deleted") {
            return Resource{kind, app->id, app->name, customer_id, workspace_id};
        }
        return std::unexpected(
            ApiResult::failure("not_found", "no such app in this workspace", std::move(path)));
    }
    return std::unexpected(ApiResult::failure(
        "invalid_input", "resource_kind must be one of: app", "/resource_kind"));
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

void resource_audit(ApiContext& context,
                    std::string kind,
                    const Resource& resource,
                    json details = json::object()) {
    context.audit_kind = std::move(kind);
    context.audit_object = resource.kind + "/" + resource.id;
    context.audit_details = {{"customer_id", resource.customer_id},
                             {"resource_kind", resource.kind},
                             {"resource_id", resource.id},
                             {"resource_name", resource.name}};
    context.audit_details.update(std::move(details));
}

std::string now_instant() {
    return control::record_instant(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
}

// agent_turn as the same person, with its own authorization, balance gate, audit and metering.
liquid::runtime::task<ApiResult> start_turn(ApiContext& context, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("store_unavailable", "the capability registry is unavailable");
    }
    ApiContext inner = context;
    inner.audit_kind.clear();
    inner.audit_details = json();
    auto done = co_await context.registry->call_async("agent_turn", input, inner);
    co_return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

std::string first_open_reason(const std::vector<control::ResourceNotification>& notifications,
                              const Resource& resource) {
    for (const auto& notification : notifications) {
        if (notification.resource_kind == resource.kind &&
            notification.resource_id == resource.id && notification.resolved_at.empty()) {
            return notification.reason;
        }
    }
    return {};
}

// Starts a repair or change turn for one resource and remembers its conversation on it.
liquid::runtime::task<ApiResult> start_resource_turn(const json& input,
                                                     ApiContext& context,
                                                     const std::string& resource_kind,
                                                     std::string turn_kind) {
    const auto repairer = repairer_of(resource_kind);
    if (!repairer) {
        co_return ApiResult::failure("invalid_input", "no agent repairs this kind of resource");
    }
    const std::string id_key(repairer->context_key);
    auto resource = owned_resource(resource_kind, input.at(id_key), input, context, "/" + id_key);
    if (!resource) {
        co_return resource.error();
    }
    auto records = workspace_records(context, resource->customer_id, resource->workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto repair = control::settled(co_await control::get_resource_repair(
        *records, resource->customer_id, resource->kind, resource->id));
    if (!repair) {
        co_return failed(repair.error());
    }
    std::string conversation_id = input.value("conversation_id", std::string());
    if (conversation_id.empty()) {
        // Later instructions continue the same conversation (lake#1207).
        conversation_id = turn_kind == "change" ? repair->change_conversation_id
                                                : repair->repair_conversation_id;
    }
    std::string message = input.value("message", std::string());
    if (message.empty() && turn_kind == "repair") {
        auto listed = control::settled(
            co_await control::list_resource_notifications(*records, resource->customer_id));
        const std::string reason = listed ? first_open_reason(*listed, *resource) : std::string();
        message = "Fix the " + resource->kind + " " + resource->name + "." +
                  (reason.empty() ? std::string() : " Lake reported: " + reason);
    }
    json handed{{id_key, resource->id}};
    if (input.contains("notification_id") && input.at("notification_id").is_string()) {
        handed["notification_id"] = input.at("notification_id");
    }
    json turn{{"customer_id", resource->customer_id},
              {"workspace_id", resource->workspace_id},
              {"agent", repairer->agent},
              {"kind", turn_kind},
              {"message", message},
              {"context", std::move(handed)}};
    if (!conversation_id.empty()) {
        turn["conversation_id"] = conversation_id;
    }
    auto awaited = co_await start_turn(context, std::move(turn));
    if (!awaited) {
        co_return ApiResult::failure("store_error", awaited.error().message);
    }
    ApiResult started = std::move(*awaited);
    if (!started.ok) {
        co_return started;
    }
    const std::string conversation = started.value.value("conversation_id", conversation_id);
    if (!conversation.empty()) {
        static_cast<void>(control::settled(co_await control::change_resource_repair(
            *records, resource->customer_id, resource->kind, resource->id,
            [&](control::ResourceRepair& value) {
                (turn_kind == "change" ? value.change_conversation_id
                                       : value.repair_conversation_id) = conversation;
            })));
    }
    resource_audit(context,
                   resource->kind + (turn_kind == "change" ? ".change.start" : ".repair.start"),
                   *resource,
                   {{"task_id", started.value.value("task_id", std::string())}});
    started.value["resource_kind"] = resource->kind;
    started.value["resource_id"] = resource->id;
    co_return started;
}

// The newest image build of an app (lake's build_app names its target
// `<registry>/<space>/<app name>:build-<id>`).
std::optional<control::Build> newest_build(const std::vector<control::Build>& builds,
                                           const control::WebApp& app) {
    std::optional<control::Build> newest;
    const std::string marker = "/" + app.name + ":";
    for (const auto& build : builds) {
        if (build.workspace_id != app.workspace_id || !build.target.contains(marker)) {
            continue;
        }
        if (!newest || build.created_at > newest->created_at) {
            newest = build;
        }
    }
    return newest;
}

bool task_finished(const control::AgentTask& task) {
    return task.phase == "Succeeded" || task.phase == "Failed" || task.desired == "absent";
}

// One resource's state as a detector saw it, which the pass turns into notifications and
// auto-fix attempts the same way for every kind.
struct Observed {
    Resource resource;
    std::int64_t generation = 0;
    std::string phase;
    std::string detail;
    std::string reason; // empty: not failing
    bool healthy = false;
    // Other notifications the detector wants recorded (an app's failed build), and the events
    // whose notifications it resolves.
    std::vector<control::ResourceNotification> extra;
    std::vector<std::string> resolves;
};

// The work of one workspace in a pass: its notifications and repair records, read once.
class WorkspacePass {
public:
    WorkspacePass(ApiContext& context,
                  control::WorkspaceRecords& records,
                  std::string customer_id,
                  ResourceRepairPass& report)
        : context_(context), records_(records), customer_id_(std::move(customer_id)),
          report_(report) {}

    bool load() {
        auto notifications = liquid::runtime::block_on(
            control::list_resource_notifications(records_, customer_id_));
        if (!notifications || !*notifications) {
            return false; // a workspace that is not ready is passed over
        }
        for (auto& notification : **notifications) {
            known_.emplace(notification.id, std::move(notification));
        }
        if (auto listed = liquid::runtime::block_on(
                control::list_resource_repairs(records_, customer_id_));
            listed && *listed) {
            for (auto& repair : **listed) {
                repairs_.emplace(control::resource_repair_key(repair.resource_kind,
                                                              repair.resource_id),
                                 std::move(repair));
            }
        }
        return true;
    }

    control::ResourceRepair repair_of(const Resource& resource) const {
        if (const auto found = repairs_.find(control::resource_repair_key(resource.kind, resource.id));
            found != repairs_.end()) {
            return found->second;
        }
        control::ResourceRepair fresh;
        fresh.resource_kind = resource.kind;
        fresh.resource_id = resource.id;
        fresh.customer_id = customer_id_;
        return fresh;
    }

    control::ResourceNotification notification(const Observed& seen,
                                               const control::ResourceRepair& repair,
                                               std::string event,
                                               std::string discriminator,
                                               std::string reason,
                                               std::string detail) const {
        control::ResourceNotification value;
        value.id = control::resource_notification_id(event, seen.resource.kind, seen.resource.id,
                                                     discriminator);
        value.customer_id = customer_id_;
        value.workspace_id = seen.resource.workspace_id;
        value.resource_kind = seen.resource.kind;
        value.resource_id = seen.resource.id;
        value.resource_name = seen.resource.name;
        value.event = std::move(event);
        value.reason = std::move(reason);
        value.detail = std::move(detail);
        value.phase = seen.phase;
        value.generation = seen.generation;
        value.recipient_user_id = repair.deployed_by;
        return value;
    }

    void record(control::ResourceNotification notification) {
        if (known_.contains(notification.id)) {
            return;
        }
        const std::string id = notification.id;
        auto done = liquid::runtime::block_on(
            control::record_resource_notification(records_, notification));
        if (done && *done) {
            if (**done) {
                ++report_.notified;
            }
            known_.emplace(id, std::move(notification));
        } else {
            report_.problems.push_back("notify " + id + ": " +
                                       (done ? (*done).error().message : done.error().message));
        }
    }

    void resolve(const Resource& resource, const std::vector<std::string>& events) {
        for (auto& [id, notification] : known_) {
            if (notification.resource_kind != resource.kind ||
                notification.resource_id != resource.id || !notification.resolved_at.empty() ||
                std::ranges::find(events, notification.event) == events.end()) {
                continue;
            }
            const std::string when = now_instant();
            auto done = liquid::runtime::block_on(control::change_resource_notification(
                records_, customer_id_, id,
                [&](control::ResourceNotification& value) { value.resolved_at = when; }));
            if (done && *done) {
                notification.resolved_at = when;
                ++report_.resolved;
            }
        }
    }

    void handle(const Observed& seen) {
        const control::ResourceRepair repair = repair_of(seen.resource);
        for (auto notification : seen.extra) {
            notification.recipient_user_id = repair.deployed_by;
            record(std::move(notification));
        }
        if (!seen.resolves.empty()) {
            resolve(seen.resource, seen.resolves);
        }
        if (seen.healthy) {
            resolve(seen.resource, {"rollout_failed", "auto_fix_stopped"});
            return;
        }
        if (seen.reason.empty()) {
            return;
        }
        record(notification(seen, repair, "rollout_failed", std::to_string(seen.generation),
                            seen.reason, seen.detail));
        auto_fix(seen, repair);
    }

private:
    void auto_fix(const Observed& seen, const control::ResourceRepair& repair) {
        const auto repairer = repairer_of(seen.resource.kind);
        if (!repairer || !repair.auto_fix || repair.auto_fix_stopped) {
            return;
        }
        // An attempt still running is left to finish; one that finished without a new
        // generation could not fix the resource.
        std::string stop;
        if (!repair.auto_fix_task_id.empty()) {
            const auto task = context_.store->get_agent_task(repair.auto_fix_task_id);
            if (task && !task_finished(*task)) {
                return;
            }
            if (repair.auto_fix_generation == seen.generation) {
                stop = "The agent's attempt did not produce a new version, so auto-fix stopped.";
            }
        }
        if (stop.empty() && std::ranges::find(repair.attempted_generations, seen.generation) !=
                                repair.attempted_generations.end()) {
            return;
        }
        if (stop.empty() && repair.attempts >= repair.max_attempts) {
            stop = "Auto-fix tried " + std::to_string(repair.attempts) + " time" +
                   (repair.attempts == 1 ? "" : "s") + " and it still fails, so it stopped.";
        }
        if (!stop.empty()) {
            const std::string why = stop;
            auto stopped = liquid::runtime::block_on(control::change_resource_repair(
                records_, customer_id_, seen.resource.kind, seen.resource.id,
                [&](control::ResourceRepair& value) {
                    value.auto_fix_stopped = true;
                    value.stopped_reason = why;
                }));
            if (stopped && *stopped) {
                ++report_.auto_fix_stopped;
                record(notification(seen, repair, "auto_fix_stopped",
                                    std::to_string(seen.generation), why + " " + seen.reason,
                                    seen.detail));
            }
            return;
        }
        // One attempt per failed generation, as the person who turned auto-fix on.
        const auto person = stream_person(context_, repair.auto_fix_set_by, "auto_fix");
        if (!person) {
            report_.problems.push_back(seen.resource.id + ": " + person.error().message);
            return;
        }
        ApiContext as = *person;
        json turn{{"customer_id", customer_id_},
                  {"workspace_id", seen.resource.workspace_id},
                  {"agent", repairer->agent},
                  {"kind", "repair"},
                  {"message", "Auto-fix: repair the " + seen.resource.kind + " " +
                                  seen.resource.name + ". Lake reported: " + seen.reason},
                  {"context",
                   {{std::string(repairer->context_key), seen.resource.id},
                    {"auto_fix", true},
                    {"notification_id",
                     control::resource_notification_id("rollout_failed", seen.resource.kind,
                                                       seen.resource.id,
                                                       std::to_string(seen.generation))}}}};
        if (!repair.repair_conversation_id.empty()) {
            turn["conversation_id"] = repair.repair_conversation_id;
        }
        auto started =
            liquid::runtime::block_on(context_.registry->call_async("agent_turn", turn, as));
        if (started && started->ok) {
            ++report_.auto_fix_started;
        } else {
            report_.problems.push_back(
                seen.resource.id + ": auto-fix did not start: " +
                (started ? started->error.message : started.error().message));
        }
    }

    ApiContext& context_;
    control::WorkspaceRecords& records_;
    std::string customer_id_;
    ResourceRepairPass& report_;
    std::map<std::string, control::ResourceNotification> known_;
    std::map<std::string, control::ResourceRepair> repairs_;
};

// The app detector: rollout state from the operator's status, and the app's newest build.
Observed observe_app(const control::WebApp& app,
                     const std::optional<std::vector<control::Build>>& builds,
                     const WorkspacePass& pass) {
    Observed seen;
    seen.resource = Resource{std::string(control::resource_kind_app), app.id, app.name,
                             app.customer_id, app.workspace_id};
    seen.generation = app.generation;
    seen.phase = app.phase;
    seen.detail = app.message;
    seen.healthy = control::app_rollout_ready(app);
    if (control::app_rollout_failed(app)) {
        seen.reason = control::app_failure_reason(app.phase, app.message);
    }
    if (builds) {
        if (const auto build = newest_build(*builds, app)) {
            if (build->phase == "Failed") {
                auto notification =
                    pass.notification(seen, pass.repair_of(seen.resource), "build_failed",
                                      build->id, control::build_failure_reason(build->message),
                                      build->message);
                notification.build_id = build->id;
                seen.extra.push_back(std::move(notification));
            } else if (build->phase == "Succeeded") {
                seen.resolves.push_back("build_failed");
            }
        }
    }
    return seen;
}

} // namespace

liquid::runtime::task<bool> note_deployment(ApiContext& context,
                                            std::string kind,
                                            std::string resource_id,
                                            std::string customer_id,
                                            std::string workspace_id,
                                            json source) {
    if (context.workspaces == nullptr || context.instance_kek.empty()) {
        co_return false;
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return false;
    }
    // An agent's task credential acts as the person who started the turn (lake#445).
    const std::string deployed_by = context.acting_user_id;
    auto written = control::settled(co_await control::change_resource_repair(
        *records, customer_id, kind, resource_id, [&](control::ResourceRepair& value) {
            if (!deployed_by.empty()) {
                value.deployed_by = deployed_by;
            }
            if (source.is_object() && !source.empty()) {
                value.source = source;
            }
        }));
    co_return written.has_value();
}

ResourceRepairPass run_resource_repair_pass(ApiContext& context) {
    ResourceRepairPass report;
    if (context.store == nullptr || context.workspaces == nullptr || context.registry == nullptr) {
        return report;
    }
    const auto customers = context.store->list_customers(100000);
    if (!customers) {
        report.problems.push_back("accounts: " + customers.error().message);
        return report;
    }
    for (const control::Customer& customer : *customers) {
        // Apps by workspace. lake#1209 adds the space's custom agents to the same walk.
        const auto apps = context.store->list_web_apps(customer.id);
        if (!apps || apps->empty()) {
            continue;
        }
        std::optional<std::vector<control::Build>> builds;
        if (auto listed = context.store->list_builds(customer.id)) {
            builds = std::move(*listed);
        }
        std::map<std::string, std::vector<control::WebApp>> by_workspace;
        for (const control::WebApp& app : *apps) {
            if (app.desired == "running" && !app.workspace_id.empty()) {
                by_workspace[app.workspace_id].push_back(app);
            }
        }
        for (const auto& [workspace_id, workspace_apps] : by_workspace) {
            auto records = workspace_records(context, customer.id, workspace_id);
            if (!records) {
                continue;
            }
            WorkspacePass pass(context, *records, customer.id, report);
            if (!pass.load()) {
                continue;
            }
            ++report.workspaces;
            for (const control::WebApp& app : workspace_apps) {
                pass.handle(observe_app(app, builds, pass));
            }
        }
    }
    return report;
}

void register_repair_capabilities(Registry& registry) {
    registry.add(Capability{
        .name = "list_notifications",
        .title = "List notifications",
        .description =
            "What Lake has told this workspace about its resources: an app whose rollout ended "
            "Degraded, failed or crash-looping, an app image build that failed, or auto-fix that "
            "stopped, each with the reason in plain words. `for_you` marks the ones for the person "
            "who deployed the resource. Filter by `resource_kind` and `resource_id`. Dismissed and "
            "resolved ones are left out unless asked for.",
        .input_schema = input_schema({{"resource_kind", resource_kind_schema()},
                                      {"resource_id", {{"type", "string"}}},
                                      {"include_dismissed", {{"type", "boolean"}}},
                                      {"include_resolved", {{"type", "boolean"}}}}),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            // A list read authorizes itself, as list_apps does.
            const auto object = customer_object(input, context);
            if (!object) {
                co_return ApiResult::failure(object.error().code, object.error().message,
                                             object.error().path);
            }
            if (context.acting_identity != "admin") {
                const auto decision = control::authorize(*context.store,
                                                         context.acting_identity,
                                                         *object,
                                                         "read",
                                                         context.authorization_cache);
                if (!decision) {
                    co_return ApiResult::failure(decision.error().code, decision.error().message);
                }
                if (!decision->allowed) {
                    co_return ApiResult::failure(
                        "unauthorized", "principal is not allowed to read this account");
                }
            }
            auto records = workspace_records(context, input.at("customer_id"),
                                             input.at("workspace_id"));
            if (!records) {
                co_return failed(records.error());
            }
            auto listed = control::settled(
                co_await control::list_resource_notifications(*records, input.at("customer_id")));
            if (!listed) {
                co_return failed(listed.error());
            }
            const std::string kind = input.value("resource_kind", std::string());
            const std::string id = input.value("resource_id", std::string());
            const bool dismissed = input.value("include_dismissed", false);
            const bool resolved = input.value("include_resolved", false);
            json items = json::array();
            std::size_t open = 0;
            for (const auto& notification : *listed) {
                if ((!kind.empty() && notification.resource_kind != kind) ||
                    (!id.empty() && notification.resource_id != id)) {
                    continue;
                }
                json value =
                    control::resource_notification_json(notification, context.acting_user_id);
                const bool was_resolved = value.at("resolved").get<bool>();
                const bool was_dismissed = value.at("dismissed").get<bool>();
                if (!was_resolved && !was_dismissed) {
                    ++open;
                }
                if ((!dismissed && was_dismissed) || (!resolved && was_resolved)) {
                    continue;
                }
                items.push_back(std::move(value));
            }
            context.audit_kind = "notifications.read";
            co_return ApiResult::success({{"notifications", std::move(items)}, {"open", open}});
        }});

    registry.add(Capability{
        .name = "dismiss_notification",
        .title = "Dismiss notification",
        .description = "Hide one notification from the person who dismisses it. Others in the "
                       "workspace still see it until its resource is healthy again.",
        .input_schema = input_schema({{"notification_id", {{"type", "string"}}}},
                                     json::array({"notification_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.acting_user_id.empty()) {
                co_return ApiResult::failure("unauthorized",
                                             "dismissing a notification needs a signed-in person");
            }
            auto records = workspace_records(context, input.at("customer_id"),
                                             input.at("workspace_id"));
            if (!records) {
                co_return failed(records.error());
            }
            const std::string user = context.acting_user_id;
            auto changed = control::settled(co_await control::change_resource_notification(
                *records, input.at("customer_id"), input.at("notification_id"),
                [&](control::ResourceNotification& value) {
                    if (std::ranges::find(value.dismissed_by, user) == value.dismissed_by.end()) {
                        value.dismissed_by.push_back(user);
                    }
                }));
            if (!changed) {
                co_return failed(changed.error());
            }
            context.audit_kind = "notification.dismiss";
            context.audit_object = changed->resource_kind + "/" + changed->resource_id;
            context.audit_details = {{"notification_id", changed->id},
                                     {"resource_kind", changed->resource_kind},
                                     {"resource_id", changed->resource_id}};
            co_return ApiResult::success(control::resource_notification_json(*changed, user));
        }});

    registry.add(Capability{
        .name = "get_auto_fix",
        .title = "Get auto-fix settings",
        .description =
            "One resource's repair settings: whether auto-fix is on, its attempts and limit, "
            "whether it stopped and why, who deployed the resource, the source it was last "
            "deployed from, and the agent conversations that repaired or changed it.",
        .input_schema = input_schema({{"resource_kind", resource_kind_schema()},
                                      {"resource_id", {{"type", "string"}}}},
                                     json::array({"resource_kind", "resource_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto resource = owned_resource(input.at("resource_kind"), input.at("resource_id"),
                                           input, context, "/resource_id");
            if (!resource) {
                co_return resource.error();
            }
            auto records =
                workspace_records(context, resource->customer_id, resource->workspace_id);
            if (!records) {
                co_return failed(records.error());
            }
            auto repair = control::settled(co_await control::get_resource_repair(
                *records, resource->customer_id, resource->kind, resource->id));
            if (!repair) {
                co_return failed(repair.error());
            }
            resource_audit(context, resource->kind + ".auto_fix.read", *resource);
            co_return ApiResult::success(control::resource_repair_json(*repair));
        }});

    registry.add(Capability{
        .name = "set_auto_fix",
        .title = "Set auto-fix",
        .description =
            "Let an agent repair a resource by itself when it fails (for an app, the Application "
            "Agent, when its rollout fails): it commits each fix to the resource's repository and "
            "redeploys, without asking, up to `max_attempts` times (default 3, at most 10), then "
            "stops and notifies. Every attempt acts as you, the person who turns it on, and is "
            "audited. Turning it on again resets the attempts. Off by default.",
        .input_schema = input_schema({{"resource_kind", resource_kind_schema()},
                                      {"resource_id", {{"type", "string"}}},
                                      {"enabled", {{"type", "boolean"}}},
                                      {"max_attempts",
                                       {{"type", "integer"},
                                        {"minimum", 1},
                                        {"maximum", control::auto_fix_most_attempts}}}},
                                     json::array({"resource_kind", "resource_id", "enabled"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            if (context.acting_user_id.empty()) {
                co_return ApiResult::failure(
                    "unauthorized", "auto-fix acts as a person: turn it on while signed in");
            }
            auto resource = owned_resource(input.at("resource_kind"), input.at("resource_id"),
                                           input, context, "/resource_id");
            if (!resource) {
                co_return resource.error();
            }
            auto records =
                workspace_records(context, resource->customer_id, resource->workspace_id);
            if (!records) {
                co_return failed(records.error());
            }
            const bool enabled = input.at("enabled").get<bool>();
            const int max_attempts = input.value("max_attempts", control::auto_fix_default_attempts);
            const std::string user = context.acting_user_id;
            auto changed = control::settled(co_await control::change_resource_repair(
                *records, resource->customer_id, resource->kind, resource->id,
                [&](control::ResourceRepair& value) {
                    value.auto_fix = enabled;
                    if (enabled) {
                        value.max_attempts = max_attempts;
                        value.attempts = 0;
                        value.attempted_generations.clear();
                        value.auto_fix_stopped = false;
                        value.stopped_reason.clear();
                        value.auto_fix_task_id.clear();
                        value.auto_fix_generation = 0;
                        value.auto_fix_set_by = user;
                    }
                }));
            if (!changed) {
                co_return failed(changed.error());
            }
            resource_audit(context,
                           resource->kind + ".auto_fix.set",
                           *resource,
                           {{"enabled", enabled}, {"max_attempts", changed->max_attempts}});
            co_return ApiResult::success(control::resource_repair_json(*changed));
        }});

    registry.add(Capability{
        .name = "repair_app",
        .title = "Fix app with the Application Agent",
        .description =
            "Ask the Application Agent to fix an app that failed. It reads the app's events, build "
            "logs, runtime logs, source and bindings, diagnoses the failure and proposes the change "
            "as a diff. Nothing is written until you confirm it on a later agent_turn (kind "
            "repair, proposal_task_id and proposal_sha256, as for any Application Agent proposal): "
            "then it commits the fix to the app's repository, rebuilds when it must, redeploys and "
            "follows the rollout. Answers the turn's task_id.",
        .input_schema = input_schema({{"app_id", {{"type", "string"}}},
                                      {"message", {{"type", "string"}}},
                                      {"notification_id", {{"type", "string"}}},
                                      {"conversation_id", {{"type", "string"}}}},
                                     json::array({"app_id"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            co_return co_await start_resource_turn(
                input, context, std::string(control::resource_kind_app), "repair");
        }});

    registry.add(Capability{
        .name = "change_app",
        .title = "Change app with the Application Agent",
        .description =
            "Ask the Application Agent to change an app from your instructions (\"add a chart by "
            "region\"). It proposes the change as a diff; confirmed on a later agent_turn (kind "
            "change), it commits it to the app's repository and redeploys. Later instructions "
            "continue the same conversation. Answers the turn's task_id.",
        .input_schema = input_schema({{"app_id", {{"type", "string"}}},
                                      {"message", {{"type", "string"}, {"minLength", 1}}},
                                      {"conversation_id", {{"type", "string"}}}},
                                     json::array({"app_id", "message"})),
        .output_schema = {{"type", "object"}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            co_return co_await start_resource_turn(
                input, context, std::string(control::resource_kind_app), "change");
        }});
}

} // namespace lake::api
