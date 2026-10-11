// A workspace's own AI solutions beside Orama's catalogue (docs/api/solutions.md, lake#597): six
// capabilities over sealed `lake_solutions` records in the workspace Zetta and the engine's
// read-only catalogue (S-1, …). list_solutions and get_solution answer both kinds, each marked
// `source: orama|workspace`; create, update and delete change only the workspace's own, and refuse
// Orama's with `forbidden`; duplicate_solution copies either kind into a new one of the
// workspace's own.

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/solutions.hpp"
#include "lake/control/store.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

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

// Read on the space, for list_solutions, which authorizes itself as every list_ read does.
std::optional<ApiResult> authorize_space_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

json string_schema() {
    return {{"type", "string"}};
}

json nullable(std::string_view type) {
    return {{"type", json::array({type, "null"})}};
}

json workspace_input(json properties, json required) {
    properties["customer_id"] = string_schema();
    properties["workspace_id"] = string_schema();
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json measurement_input() {
    return {
        {"type", "object"},
        {"description",
         "how its return is measured: the metric, and optionally where it stands today "
         "(baseline) and where it should go (target)"},
        {"properties",
         {{"metric", string_schema()}, {"baseline", string_schema()}, {"target", string_schema()}}},
        {"required", json::array({"metric"})},
        {"additionalProperties", false}};
}

json methodologies_input() {
    return {{"type", "array"},
            {"description",
             "the methodologies it composes: ids such as \"M-1\", or {id, minimum_level} where "
             "minimum_level (1 to 4, default 1) is the level from which it is used"},
            {"items",
             {{"type", json::array({"string", "object"})},
              {"properties",
               {{"id", string_schema()},
                {"minimum_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}}}},
              {"required", json::array({"id"})},
              {"additionalProperties", false}}}};
}

// The fields a person authors, shared by create_solution and update_solution.
json authored_fields() {
    return {{"name", string_schema()},
            {"problem", {{"type", "string"}, {"description", "the business problem it answers"}}},
            {"decision", {{"type", "string"}, {"description", "the decision it drives"}}},
            {"measurement", measurement_input()},
            {"methodologies", methodologies_input()},
            {"pipeline",
             {{"type", "string"},
              {"description",
               "an optional pipeline template or reference, such as <project>/<pipeline>"}}},
            {"notes", string_schema()}};
}

// One solution in an answer: Orama's carry the engine's catalogue fields; a workspace's carry the
// fields below. Both carry `id`, `source`, `editable` and `name`.
json solution_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", string_schema()},
              {"source", {{"type", "string"}, {"enum", {"orama", "workspace"}}}},
              {"editable", {{"type", "boolean"}}},
              {"name", string_schema()},
              {"problem", string_schema()},
              {"decision", string_schema()},
              {"measurement", {{"type", "object"}}},
              {"methodologies", {{"type", "array"}}},
              {"pipeline", nullable("string")},
              {"notes", nullable("string")},
              {"duplicated_from", nullable("string")},
              {"created_at", string_schema()},
              {"created_by", string_schema()},
              {"created_by_name", nullable("string")},
              {"updated_at", nullable("string")},
              {"updated_by", nullable("string")}}},
            {"required", json::array({"id", "source", "editable", "name"})}};
}

json or_null(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

// The author's name: the display name of the user `created_by` names, else their email, from the
// control store, where identity lives; null for an author lake has no user for. Each author is
// read once per call.
class AuthorNames {
public:
    explicit AuthorNames(ApiContext& context) : context_(context) {}

    json operator()(const std::string& user_id) {
        if (const auto known = names_.find(user_id); known != names_.end()) {
            return known->second;
        }
        json name = nullptr;
        if (context_.store && !user_id.empty()) {
            if (const auto user = context_.store->get_user(user_id); user) {
                if (!user->display_name.empty()) {
                    name = user->display_name;
                } else if (!user->email.empty()) {
                    name = user->email;
                }
            }
        }
        names_.emplace(user_id, name);
        return name;
    }

private:
    ApiContext& context_;
    std::map<std::string, json> names_;
};

json workspace_json(const control::WorkspaceSolution& solution, AuthorNames& names) {
    json methodologies = json::array();
    for (const control::SolutionMethodology& methodology : solution.methodologies) {
        methodologies.push_back(
            {{"id", methodology.id}, {"minimum_level", methodology.minimum_level}});
    }
    return {{"id", solution.id},
            {"source", "workspace"},
            {"editable", true},
            {"name", solution.name},
            {"problem", solution.problem},
            {"decision", solution.decision},
            {"measurement",
             {{"metric", solution.measurement.metric},
              {"baseline", or_null(solution.measurement.baseline)},
              {"target", or_null(solution.measurement.target)}}},
            {"methodologies", std::move(methodologies)},
            {"pipeline", or_null(solution.pipeline)},
            {"notes", or_null(solution.notes)},
            {"duplicated_from", or_null(solution.duplicated_from)},
            {"created_at", solution.created_at},
            {"created_by", solution.created_by},
            {"created_by_name", names(solution.created_by)},
            {"updated_at", or_null(solution.updated_at)},
            {"updated_by", or_null(solution.updated_by)}};
}

// One of Orama's catalogue entries as the engine answered it, marked read-only.
json orama_json(json entry) {
    if (!entry.is_object()) {
        return json(nullptr);
    }
    entry["source"] = "orama";
    entry["editable"] = false;
    return entry;
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

std::string trimmed(std::string text) {
    const auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && blank(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    text.erase(text.begin(), std::ranges::find_if_not(text, blank));
    return text;
}

ApiResult refused(const control::SolutionProblem& problem) {
    return ApiResult::failure("invalid_input", problem.message, problem.path);
}

control::SolutionMeasurement measurement_of(const json& value) {
    return {trimmed(value.value("metric", std::string())),
            trimmed(value.value("baseline", std::string())),
            trimmed(value.value("target", std::string()))};
}

std::vector<control::SolutionMethodology> methodologies_of(const json& value) {
    std::vector<control::SolutionMethodology> out;
    for (const json& item : value) {
        if (item.is_string()) {
            out.push_back({trimmed(item.get<std::string>()), 1});
        } else if (item.is_object()) {
            out.push_back(
                {trimmed(item.value("id", std::string())), item.value("minimum_level", 1)});
        }
    }
    return out;
}

// The engine's catalogue read in one workspace (`list_solutions` or `get_solution`).
liquid::runtime::task<control::ZettaControlResult<json>> catalogue(ApiContext& context,
                                                                   std::string customer_id,
                                                                   std::string workspace_id,
                                                                   std::string capability,
                                                                   json input) {
    if (!context.workspaces) {
        co_return control::ZettaControlResult<json>(std::unexpected(
            control::ZettaControlError{"workspace_not_ready", "a workspace Zetta is required"}));
    }
    ApiContext bound = context;
    bound.workspace_id = std::move(workspace_id);
    co_return control::settled(co_await call_workspace(
        bound, std::move(customer_id), std::move(capability), true, std::move(input)));
}

// One of Orama's catalogue solutions by id: the entry, nullopt when the catalogue has none by that
// id, or why it could not be read.
liquid::runtime::task<control::ZettaControlResult<std::optional<json>>> catalogue_entry(
    ApiContext& context, std::string customer_id, std::string workspace_id, std::string id) {
    using Found = std::optional<json>;
    auto got = control::settled(co_await catalogue(context,
                                                   std::move(customer_id),
                                                   std::move(workspace_id),
                                                   "get_solution",
                                                   {{"solution", id}}));
    if (!got) {
        if (got.error().code == "not_found") {
            co_return control::ZettaControlResult<Found>(Found{});
        }
        co_return control::ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    const json entry = got->is_object() && got->contains("solution") ? got->at("solution") : *got;
    if (!entry.is_object() || entry.value("id", std::string()).empty()) {
        co_return control::ZettaControlResult<Found>(Found{});
    }
    co_return control::ZettaControlResult<Found>(Found{entry});
}

// Why Orama's solution `id` cannot be changed: it is the catalogue's, and read-only. The catalogue
// is read only to name it; an id it does not know is not found.
liquid::runtime::task<ApiResult> refuse_orama_change(ApiContext& context,
                                                     const std::string& customer_id,
                                                     const std::string& workspace_id,
                                                     const std::string& id,
                                                     std::string_view verb) {
    auto entry = control::settled(co_await catalogue_entry(context, customer_id, workspace_id, id));
    if (entry && !*entry) {
        co_return ApiResult::failure("not_found", "no such solution in this workspace", "/id");
    }
    std::string named = id;
    if (entry && *entry) {
        if (const std::string name = (**entry).value("name", std::string()); !name.empty()) {
            named += " (" + name + ")";
        }
    }
    co_return ApiResult::failure(
        "forbidden",
        named + " is one of Orama's catalogue solutions, which are read-only and cannot be " +
            std::string(verb) +
            ". Duplicate it (duplicate_solution) to make an editable solution of your own.",
        "/id");
}

std::string acting_author(const ApiContext& context) {
    return context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id;
}

liquid::runtime::task<ApiResult> list_solutions(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string source = input.value("source", std::string());
    json solutions = json::array();
    json catalogue_state{{"available", true}, {"message", nullptr}};
    if (source.empty() || source == "orama") {
        auto listed = control::settled(
            co_await catalogue(context, customer_id, workspace_id, "list_solutions", {}));
        const json entries = listed && listed->is_object()
                                 ? listed->value("solutions", json::array())
                                 : json::array();
        if (!listed) {
            // The workspace's own solutions are still listed; the page says why Orama's are not.
            catalogue_state = {
                {"available", false},
                {"message",
                 listed.error().code == "unknown_capability"
                     ? "this workspace's engine does not list Orama's solutions "
                       "yet"
                     : "Orama's solutions could not be read: " + listed.error().message}};
        }
        for (const json& entry : entries) {
            if (json marked = orama_json(entry); !marked.is_null()) {
                solutions.push_back(std::move(marked));
            }
        }
    }
    if (source.empty() || source == "workspace") {
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            co_return failed(records.error());
        }
        auto listed =
            control::settled(co_await control::list_workspace_solutions(*records, customer_id));
        if (!listed) {
            co_return failed(listed.error());
        }
        AuthorNames names(context);
        for (const control::WorkspaceSolution& solution : *listed) {
            solutions.push_back(workspace_json(solution, names));
        }
    }
    co_return ApiResult::success(
        {{"solutions", std::move(solutions)}, {"catalogue", std::move(catalogue_state)}});
}

liquid::runtime::task<ApiResult> get_solution(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    if (control::workspace_solution_id(id)) {
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            co_return failed(records.error());
        }
        auto found =
            control::settled(co_await control::get_workspace_solution(*records, customer_id, id));
        if (!found) {
            co_return failed(found.error());
        }
        if (!*found) {
            co_return ApiResult::failure("not_found", "no such solution in this workspace", "/id");
        }
        AuthorNames names(context);
        co_return ApiResult::success({{"solution", workspace_json(**found, names)}});
    }
    auto entry = control::settled(co_await catalogue_entry(context, customer_id, workspace_id, id));
    if (!entry) {
        co_return failed(entry.error());
    }
    if (!*entry) {
        co_return ApiResult::failure(
            "not_found", "Orama's catalogue lists no solution " + id, "/id");
    }
    co_return ApiResult::success({{"solution", orama_json(**entry)}});
}

liquid::runtime::task<ApiResult> create_solution(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    control::WorkspaceSolution solution;
    solution.customer_id = customer_id;
    solution.name = trimmed(input.at("name"));
    solution.problem = trimmed(input.at("problem"));
    solution.decision = trimmed(input.at("decision"));
    solution.measurement = measurement_of(input.at("measurement"));
    solution.methodologies = methodologies_of(input.value("methodologies", json::array()));
    solution.pipeline = trimmed(input.value("pipeline", std::string()));
    solution.notes = trimmed(input.value("notes", std::string()));
    solution.created_by = acting_author(context);
    if (auto problem = control::solution_problem(solution)) {
        co_return refused(*problem);
    }
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto stored =
        control::settled(co_await control::record_solution(*records, std::move(solution)));
    if (!stored) {
        co_return failed(stored.error());
    }
    context.audit_details["solution_id"] = stored->id;
    AuthorNames names(context);
    co_return ApiResult::success({{"solution", workspace_json(*stored, names)}});
}

liquid::runtime::task<ApiResult> update_solution(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["solution_id"] = id;
    if (!control::workspace_solution_id(id)) {
        co_return co_await refuse_orama_change(context, customer_id, workspace_id, id, "edited");
    }
    control::SolutionChange change;
    const auto text = [&](const char* key) -> std::optional<std::string> {
        if (!input.contains(key) || input.at(key).is_null()) {
            return std::nullopt;
        }
        return trimmed(input.at(key));
    };
    change.name = text("name");
    change.problem = text("problem");
    change.decision = text("decision");
    change.pipeline = text("pipeline");
    change.notes = text("notes");
    if (input.contains("measurement") && !input.at("measurement").is_null()) {
        change.measurement = measurement_of(input.at("measurement"));
    }
    if (input.contains("methodologies") && !input.at("methodologies").is_null()) {
        change.methodologies = methodologies_of(input.at("methodologies"));
    }
    change.updated_by = acting_author(context);
    if (control::empty_change(change)) {
        co_return ApiResult::failure("invalid_input", "send at least one field to change", "/name");
    }
    // Checked here too so a refusal names its field before any workspace is read.
    control::WorkspaceSolution probe{.customer_id = customer_id,
                                     .name = "x",
                                     .problem = "x",
                                     .decision = "x",
                                     .measurement = {"x", "", ""}};
    control::apply_change(probe, change, 0);
    if (auto problem = control::solution_problem(probe)) {
        co_return refused(*problem);
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto edited = control::settled(
        co_await control::edit_workspace_solution(*records, customer_id, id, std::move(change)));
    if (!edited) {
        if (edited.error().code == "not_found") {
            co_return ApiResult::failure("not_found", "no such solution in this workspace", "/id");
        }
        co_return failed(edited.error());
    }
    AuthorNames names(context);
    co_return ApiResult::success({{"solution", workspace_json(*edited, names)}});
}

liquid::runtime::task<ApiResult> delete_solution(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    context.audit_details["solution_id"] = id;
    if (!control::workspace_solution_id(id)) {
        co_return co_await refuse_orama_change(context, customer_id, workspace_id, id, "deleted");
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    auto removed =
        control::settled(co_await control::remove_workspace_solution(*records, customer_id, id));
    if (!removed) {
        if (removed.error().code == "not_found") {
            co_return ApiResult::failure("not_found", "no such solution in this workspace", "/id");
        }
        co_return failed(removed.error());
    }
    co_return ApiResult::success({{"deleted", true}});
}

liquid::runtime::task<ApiResult> duplicate_solution(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    const std::string id = input.at("id");
    const std::string name = trimmed(input.value("name", std::string()));
    if (name.size() > control::max_solution_name_bytes) {
        co_return ApiResult::failure("invalid_input", "a name is longer than 200 bytes", "/name");
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    control::WorkspaceSolution copy;
    if (control::workspace_solution_id(id)) {
        auto found =
            control::settled(co_await control::get_workspace_solution(*records, customer_id, id));
        if (!found) {
            co_return failed(found.error());
        }
        if (!*found) {
            co_return ApiResult::failure("not_found", "no such solution in this workspace", "/id");
        }
        copy = std::move(**found);
        copy.name = name.empty() ? control::duplicate_name(copy.name) : name;
        copy.duplicated_from = id;
        copy.updated_by.clear();
        copy.updated_at.clear();
    } else {
        auto entry =
            control::settled(co_await catalogue_entry(context, customer_id, workspace_id, id));
        if (!entry) {
            co_return failed(entry.error());
        }
        if (!*entry) {
            co_return ApiResult::failure(
                "not_found", "Orama's catalogue lists no solution " + id, "/id");
        }
        copy = control::solution_from_catalogue(**entry, name);
    }
    copy.id.clear();
    copy.customer_id = customer_id;
    copy.created_by = acting_author(context);
    auto stored = control::settled(co_await control::record_solution(*records, std::move(copy)));
    if (!stored) {
        co_return failed(stored.error());
    }
    context.audit_details["solution_id"] = stored->id;
    context.audit_details["duplicated_from"] = id;
    AuthorNames names(context);
    co_return ApiResult::success({{"solution", workspace_json(*stored, names)}});
}

} // namespace

void register_solution_capabilities(Registry& registry) {
    const json one_solution{{"type", "object"},
                            {"properties", {{"solution", solution_schema()}}},
                            {"required", json::array({"solution"})},
                            {"additionalProperties", false}};
    const json id{{"type", "string"},
                  {"description",
                   "a solution's id: S-1, … for Orama's, a UUID for one of the workspace's own"}};
    registry.add(Capability{
        .name = "list_solutions",
        .title = "List Solutions",
        .description =
            "The AI solutions a workspace can start: Orama's catalogue (read-only, `source: "
            "orama`, ids S-1, …) and the workspace's own (`source: workspace`, editable), each "
            "with `editable`. `source` keeps one kind. When the workspace's engine cannot list "
            "Orama's, `catalogue.available` is false and `catalogue.message` says why; the "
            "workspace's own are still listed.",
        .input_schema = workspace_input(
            {{"source", {{"type", "string"}, {"enum", {"orama", "workspace"}}}}}, json::array()),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"solutions", {{"type", "array"}, {"items", solution_schema()}}},
                            {"catalogue",
                             {{"type", "object"},
                              {"properties",
                               {{"available", {{"type", "boolean"}}},
                                {"message", nullable("string")}}},
                              {"required", json::array({"available", "message"})},
                              {"additionalProperties", false}}}}},
                          {"required", json::array({"solutions", "catalogue"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = list_solutions});
    registry.add(Capability{
        .name = "get_solution",
        .title = "Read a Solution",
        .description = "One solution, Orama's or the workspace's own, marked with `source` and "
                       "`editable`.",
        .input_schema = workspace_input({{"id", id}}, json::array({"id"})),
        .output_schema = one_solution,
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .async_handler = get_solution});
    registry.add(Capability{
        .name = "create_solution",
        .title = "Create a solution",
        .description =
            "Create an AI solution of the workspace's own, in the catalogue's shape: its name, "
            "the business problem, the decision it drives, how its return is measured (metric, "
            "optional baseline and target), the methodologies it composes, an optional pipeline "
            "template or reference, and notes. It is kept as a sealed record in the workspace "
            "Zetta. Start it as a project from its page.",
        .input_schema = workspace_input(
            authored_fields(), json::array({"name", "problem", "decision", "measurement"})),
        .output_schema = one_solution,
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = create_solution});
    json update_fields = authored_fields();
    update_fields["id"] = id;
    registry.add(Capability{
        .name = "update_solution",
        .title = "Edit a solution",
        .description =
            "Change one of the workspace's own solutions; the fields sent replace theirs, the "
            "others are kept. Orama's catalogue solutions are read-only: `forbidden`, with the "
            "reason (duplicate one with duplicate_solution to edit it).",
        .input_schema = workspace_input(std::move(update_fields), json::array({"id"})),
        .output_schema = one_solution,
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = update_solution});
    registry.add(Capability{
        .name = "delete_solution",
        .title = "Delete a solution",
        .description = "Delete one of the workspace's own solutions; it cannot be restored. "
                       "Projects started from it are kept. Orama's catalogue solutions are "
                       "read-only: `forbidden`, with the reason.",
        .input_schema = workspace_input({{"id", id}}, json::array({"id"})),
        .output_schema = {{"type", "object"},
                          {"properties", {{"deleted", {{"type", "boolean"}, {"const", true}}}}},
                          {"required", json::array({"deleted"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = delete_solution});
    registry.add(Capability{
        .name = "duplicate_solution",
        .title = "Duplicate a solution",
        .description =
            "Copy one of Orama's catalogue solutions, or one of the workspace's own, into a new "
            "editable solution of the workspace's own, named `name` or \"<name> (copy)\". From "
            "Orama's: the problem is its use case, the decision its promise, the metric its "
            "measured outcome, the methodologies its composition and the pipeline its "
            "methodologies in order; the notes say where it came from.",
        .input_schema =
            workspace_input({{"id", id}, {"name", string_schema()}}, json::array({"id"})),
        .output_schema = one_solution,
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = duplicate_solution});
}

} // namespace lake::api
