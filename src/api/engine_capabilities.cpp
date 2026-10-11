// The workspace engine's capabilities as named Lake capabilities (lake#650,
// docs/api/engine-capabilities.md). Every name in the workspace_reads and workspace_actions
// allow-lists is reached by name: through a Lake capability that already covers it, or through one
// registered here from the engine's own input schema (src/api/engine_openapi.json, vendored by
// tools/api/vendor-engine-openapi.py). Each call runs through call_workspace_capability, the path
// workspace_query and workspace_act take, so the policy, the agent scopes, the audit and the
// workspace caller's caching are the same.
#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/capabilities.hpp"
#include "lake/control/policy.hpp"

namespace lake::api {

// generated/engine_openapi.cpp (cmake/embed_engine_openapi.cmake)
std::string_view engine_openapi_snapshot();

namespace {

using json = nlohmann::json;

struct EngineName {
    std::string_view engine; // the engine's capability, as the allow-list names it
    std::string_view lake;   // the Lake capability that serves it
    std::string_view group;  // llms.txt's group; empty when an existing Lake capability covers it
};

// Every allow-listed engine capability, in the allow-lists' order. A name with no group is
// covered by the Lake capability named beside it, which already existed and does the same job
// (often more: list_runs filters, get_pipeline adds each step's status). Engine names that
// collide with a Lake capability that does something else, or that say too little on their own
// (`run`, `gate`, `deploy`, `chat`), get a clearer name. Lake says experiment where the engine
// says study (lake#561).
constexpr auto engine_names = std::to_array<EngineName>({
    // Reads.
    {"server_info", "workspace_engine_info", "Engine"},
    {"get_run_metrics", "get_run_metrics", ""},
    {"compare_runs", "compare_runs", ""},
    {"get_study", "get_experiment", ""},
    {"list_studies", "list_experiments", ""},
    {"list_trials", "list_experiment_trials", ""},
    {"get_study_analysis", "get_experiment_analysis", ""},
    {"browse_topic", "browse_stream", ""},
    {"score", "score", "Models"},
    {"explain", "explain", "Models"},
    {"list_projects", "list_projects", "Projects and pipelines"},
    {"get_project", "get_project", "Projects and pipelines"},
    {"get_graph", "get_pipeline", ""},
    {"list_connections", "list_connections", "Connections"},
    {"list_datasets", "list_datasets", ""},
    {"get_dataset", "get_dataset", ""},
    {"list_runs", "list_runs", ""},
    {"get_run", "get_run", "Training and runs"},
    {"get_run_report", "get_run_report", "Training and runs"},
    {"list_models", "list_models", "Models"},
    {"get_model", "get_model", ""},
    {"model_health", "model_health", "Models"},
    {"list_indexes", "list_indexes", "Search indexes"},
    {"get_index", "get_index", "Search indexes"},
    {"search", "search_workspace_index", "Search indexes"},
    {"list_jobs", "list_jobs", "Training and runs"},
    {"get_job", "get_job", "Training and runs"},
    {"get_trace", "get_trace", "Engine"},
    {"list_traces", "list_traces", "Engine"},
    {"list_source_tables", "list_source_tables", "Connections"},
    {"list_dataset_generators", "list_dataset_generators", "Datasets"},
    {"list_agents", "list_agents", "Agents"},
    {"get_agent", "get_agent", "Agents"},
    {"agent_training_status", "agent_training_status", "Agents"},
    {"list_topics", "list_streams", ""},
    {"receive_events", "browse_stream", ""},
    {"list_methodologies", "list_methodologies", "Projects and pipelines"},
    {"validate_study", "validate_experiment", "Experiments"},
    {"analyze_dataset", "analyze_dataset", "Datasets"},
    {"plan_features", "plan_features", "Datasets"},
    {"list_solutions", "list_solutions", ""},
    {"get_solution", "get_solution", ""},
    {"get_policy", "get_policy", "Decisions"},
    {"get_experiment", "get_holdout_experiment", "Decisions"},
    {"list_ledger", "list_ledger", "Decisions"},
    {"get_monitor", "get_monitor", "Decisions"},
    {"get_roi_readout", "get_roi_readout", "Decisions"},
    // Actions.
    {"create_project", "create_project", "Projects and pipelines"},
    {"create_study", "start_experiment", ""},
    {"stop_study", "stop_experiment", ""},
    {"stop_trial", "stop_experiment_trial", ""},
    {"promote_trial", "promote_trial", ""},
    {"create_solution", "compose_solution", "Solutions"},
    {"run_solution", "run_solution", "Solutions"},
    {"update_policy", "update_policy", "Decisions"},
    {"decide", "decide", "Decisions"},
    {"approve_decision", "approve_decision", "Decisions"},
    {"record_outcome", "record_outcome", "Decisions"},
    {"capture_intent", "capture_intent", "Projects and pipelines"},
    {"compile_intent", "compile_intent", "Projects and pipelines"},
    {"approve_plan", "approve_pipeline", "Projects and pipelines"},
    {"gate", "check_pipeline", "Projects and pipelines"},
    {"run", "start_run", "Training and runs"},
    {"create_connection", "create_connection", "Connections"},
    {"delete_connection", "delete_connection", "Connections"},
    {"create_dataset_from_query", "create_dataset_from_query", "Datasets"},
    {"upload_dataset", "upload_dataset", "Datasets"},
    {"delete_project", "delete_project", "Projects and pipelines"},
    {"create_index", "create_index", "Search indexes"},
    {"expose_model", "expose_model", "Models"},
    {"unexpose_model", "unexpose_model", "Models"},
    {"expose_index", "expose_index", "Search indexes"},
    {"unexpose_index", "unexpose_index", "Search indexes"},
    {"save_graph", "save_pipeline", "Projects and pipelines"},
    {"deploy", "deploy_pipeline", "Projects and pipelines"},
    {"import_n8n", "import_n8n", "Projects and pipelines"},
    {"compile_n8n", "compile_n8n", "Projects and pipelines"},
    {"compile_python", "compile_python", "Projects and pipelines"},
    {"promote_dataset", "promote_dataset", "Datasets"},
    {"generate_dataset", "generate_dataset", "Datasets"},
    {"create_agent", "create_agent", "Agents"},
    {"update_agent", "update_agent", "Agents"},
    {"delete_agent", "delete_agent", "Agents"},
    {"set_training_schedule", "set_training_schedule", "Agents"},
    {"agent_run", "agent_run", "Agents"},
    {"chat", "chat_with_agent", "Agents"},
});

const EngineName* by_engine(std::string_view engine) {
    const auto found = std::ranges::find(engine_names, engine, &EngineName::engine);
    return found == engine_names.end() ? nullptr : &*found;
}

const EngineName* registered(std::string_view lake) {
    const auto found = std::ranges::find_if(engine_names, [&](const EngineName& name) {
        return name.lake == lake && !name.group.empty();
    });
    return found == engine_names.end() ? nullptr : &*found;
}

// The entity a capability addresses has one name in Lake (docs/api/naming.md): `model_id`,
// `index_id`, `run_id`, `job_id`. Where the engine says `model` or `index`, the named capability
// publishes the `_id` field and accepts the engine's spelling as a deprecated alias;
// canonical_workspace_input sends the engine the one it takes.
struct EntityField {
    std::string_view engine, field;
};
constexpr auto entity_fields = std::to_array<EntityField>({
    {"get_run", "run_id"},
    {"get_run_report", "run_id"},
    {"model_health", "model_id"},
    {"expose_model", "model"},
    {"unexpose_model", "model"},
    {"score", "model"},
    {"explain", "model"},
    {"get_index", "index_id"},
    {"expose_index", "index"},
    {"unexpose_index", "index"},
    {"get_job", "job_id"},
    // An agent's model: a model id, a checkpoint or a Hugging Face repository id.
    {"create_agent", "model"},
    {"update_agent", "model"},
});

// What Lake adds to or checks in an engine call, said beside the engine's own description.
std::string lake_note(std::string_view engine) {
    if (engine == "expose_model" || engine == "expose_index") {
        return "Lake answers `call`, the Lake capability that serves what was published "
               "(predict for a model, search_index for an index).";
    }
    if (engine == "create_agent" || engine == "update_agent") {
        return "A Python agent's `definition` is `{source: \"zetta\", instructions, entrypoint, "
               "files: [{path, contents}]}` or `{source: \"lake\", repository_id, revision, "
               "entrypoint}`; Lake refuses any other field. A native agent created without "
               "`model_id` runs Lake's base model.";
    }
    if (engine == "create_project") {
        return "`decision` (optional) is the decision the project serves; Lake keeps it in the "
               "workspace and get_project answers it. A name in use is refused as a conflict.";
    }
    if (engine == "upload_dataset" || engine == "create_dataset_from_query") {
        return "A name already taken is refused unless `if_exists` is \"rename\", which saves under "
               "the next free name and answers `renamed_from`; the answer carries the dataset's "
               "`id` (<database>/<name>).";
    }
    if (engine == "compile_intent") {
        return "Lake's compile_pipeline captures a description, compiles it and points the new "
               "pipeline at a dataset in one call.";
    }
    if (engine == "run") {
        return "run_pipeline checks a pipeline and runs it in one call; start_run runs one that "
               "already passed, with every runner setting the engine takes.";
    }
    if (engine == "search") {
        return "Searches any index of the workspace, published or not; search_index searches a "
               "published one.";
    }
    if (engine == "create_solution") {
        return "Lake's create_solution keeps a workspace's own solution; this composes a "
               "catalogue solution's governed foundation into a project, as Start as a Project "
               "does.";
    }
    if (engine == "get_agent" || engine == "agent_training_status") {
        return "`id` is the agent's id, as list_agents answers it.";
    }
    if (engine == "get_experiment") {
        return "The solutions holdout experiment, not a training experiment (get_experiment).";
    }
    return {};
}

// The engine's description in a Lake caller's terms: issue references and permission notes
// removed (as workspace_openapi does), the engine's names for what Lake renamed replaced by
// Lake's, and the node and cluster said as the workspace.
std::string lake_terms(std::string text) {
    text = engine_words_in_lake_terms(std::move(text));
    text = std::regex_replace(text, std::regex(R"( ?\(#[0-9]+[^)]*\))"), "");
    text = std::regex_replace(text, std::regex(R"( ?Requires [a-z0-9_]+:[a-z0-9_]+\.)"), "");
    text = std::regex_replace(text, std::regex(R"(workspace_act (with capability )?)"), "");
    // One pass, so a name Lake gives one capability (get_experiment) is never renamed again as
    // the engine's name for another.
    std::string renamed;
    const std::regex word(R"(`?[a-z][a-z0-9_]*`?)");
    std::size_t copied = 0;
    for (auto at = std::sregex_iterator(text.begin(), text.end(), word);
         at != std::sregex_iterator(); ++at) {
        const std::string token = at->str();
        const bool quoted = token.size() > 2 && token.front() == '`' && token.back() == '`';
        const std::string bare = quoted ? token.substr(1, token.size() - 2) : token;
        const EngineName* name = by_engine(bare);
        // A one-word name (`run`, `gate`, `search`) is renamed only where it is quoted as one.
        if (name == nullptr || name->engine == name->lake ||
            (!quoted && bare.find('_') == std::string::npos)) {
            continue;
        }
        renamed += text.substr(copied, static_cast<std::size_t>(at->position()) - copied);
        renamed += quoted ? "`" + std::string(name->lake) + "`" : std::string(name->lake);
        copied = static_cast<std::size_t>(at->position() + at->length());
    }
    text = renamed + text.substr(copied);
    // A person runs experiments; study is only the engine's word (lake#561).
    text = std::regex_replace(text, std::regex(R"(\bstudies\b)"), "experiments");
    text = std::regex_replace(text, std::regex(R"(\bstudy\b)"), "experiment");
    text = std::regex_replace(text, std::regex(R"(\bStudy\b)"), "Experiment");
    for (const auto& [from, to] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"on every node of the cluster", "across the workspace"},
             {"on every node", "across the workspace"},
             {"from every node", "from the workspace"},
             {"to every node", "across the workspace"},
             {"on this node", "in this workspace"},
             {"this node's", "this workspace's"},
         })) {
        for (std::size_t at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }
    return text;
}

json identifier(std::string_view description) {
    return {{"type", "string"}, {"minLength", 1}, {"description", description}};
}

// The named capability's input: the engine's schema, the entity named as Lake names it, Lake's own
// fields, and the customer and workspace every workspace capability takes.
json lake_input_schema(std::string_view engine,
                       json schema,
                       std::vector<std::pair<std::string, std::string>>& aliases) {
    if (!schema.is_object() || schema.value("type", std::string()) != "object") {
        schema = {{"type", "object"}};
    }
    json properties = schema.value("properties", json::object());
    json required = schema.value("required", json::array());
    if (const auto entity = std::ranges::find(entity_fields, engine, &EntityField::engine);
        entity != entity_fields.end()) {
        const std::string field(entity->field);
        const bool engine_says_id = field.ends_with("_id");
        const std::string lake_field = engine_says_id ? field : field + "_id";
        const std::string other = engine_says_id ? field.substr(0, field.size() - 3) : field;
        if (!engine_says_id && properties.contains(field)) {
            properties[lake_field] = std::move(properties[field]);
            properties.erase(field);
            for (auto& name : required) {
                if (name == field) {
                    name = lake_field;
                }
            }
        }
        aliases.emplace_back(other, lake_field);
    }
    if (engine == "create_project") {
        properties["decision"] = {
            {"type", json::array({"string", "null"})},
            {"description", "the decision this project serves, kept by Lake in the workspace"}};
    }
    if (engine == "create_dataset_from_query" && !properties.contains("if_exists")) {
        properties["if_exists"] = {
            {"type", "string"},
            {"enum", json::array({"fail", "rename"})},
            {"description", "\"rename\" saves under the next free name when the name is taken"}};
    }
    properties["customer_id"] = identifier("the account whose workspace this is");
    properties["workspace_id"] = identifier("the workspace whose engine answers");
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    schema["additionalProperties"] = false;
    return schema;
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

// What the registry checks for any other capability, for a list that authorizes its own
// candidates: `read` on the account.
std::optional<ApiResult> refuse_unless_reader(const json& input, ApiContext& context) {
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

// A capability's title: the engine's summary, or its name said as words.
std::string title_of(const EngineName& name, const json& operation) {
    if (name.engine == name.lake) {
        if (std::string summary = operation.value("summary", std::string()); !summary.empty()) {
            return summary;
        }
    }
    std::string title(name.lake);
    std::ranges::replace(title, '_', ' ');
    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    return title;
}

} // namespace

std::string_view engine_capability_name(std::string_view engine) {
    const EngineName* name = by_engine(engine);
    return name ? name->lake : std::string_view();
}

std::string_view engine_capability_of(std::string_view name) {
    const EngineName* found = registered(name);
    return found ? found->engine : std::string_view();
}

std::string_view engine_capability_group(std::string_view name) {
    const EngineName* found = registered(name);
    return found ? found->group : std::string_view();
}

void register_engine_capabilities(Registry& registry) {
    const json snapshot = json::parse(engine_openapi_snapshot(), nullptr, false);
    const json operations = snapshot.is_object()
                                ? snapshot.value("capabilities", json::object())
                                : json::object();
    for (const EngineName& name : engine_names) {
        if (name.group.empty()) {
            continue;
        }
        const std::string engine(name.engine);
        // `score` and `explain` are in both lists; the engine takes them with GET and they change
        // nothing, so they are reads. `chat` is a GET in the engine but loads a model: an action.
        const bool read = workspace_read_allowed(engine) && engine != "chat";
        const auto operation = operations.find(engine);
        const bool described = operation != operations.end() && operation->is_object();
        std::vector<std::pair<std::string, std::string>> aliases;
        json schema = lake_input_schema(
            engine,
            described ? operation->value("input_schema", json::object()) : json::object(),
            aliases);
        if (!described) {
            // Nothing invented: the caller's fields go to the engine as sent.
            schema.erase("additionalProperties");
        }
        std::string description =
            described ? lake_terms(operation->value("description", std::string())) : std::string();
        if (!described) {
            description = "The workspace engine publishes no input schema for " + engine +
                          "; the fields are passed to it as sent.";
        }
        if (const std::string note = lake_note(engine); !note.empty()) {
            description += " " + note;
        }
        // Lake never names a study, not even as the engine's name (lake#561).
        if (name.engine != name.lake && !engine.contains("stud")) {
            description += " The engine calls it " + engine +
                           (read ? "; workspace_query" : "; workspace_act") +
                           " takes that name too.";
        }
        registry.add(Capability{
            .name = std::string(name.lake),
            .title = title_of(name, described ? *operation : json::object()),
            .description = std::move(description),
            .input_schema = std::move(schema),
            .output_schema = {{"type", "object"}},
            .kind = read ? CapabilityKind::Read : CapabilityKind::Action,
            // A read's input can be large or nested (validate_experiment's search space): a JSON
            // body is taken as well as the query string.
            .post_alias = read,
            .object = "customer",
            .action = read ? "read" : "write",
            .resolve_object = customer_object,
            // Lake's list capabilities authorize their own candidates; an engine list's one
            // candidate is the account, read below exactly as the registry would.
            .authorizes_candidates = name.lake.starts_with("list_"),
            .async_handler = [engine, read, list = name.lake.starts_with("list_")](
                                 const json& input,
                                 ApiContext& context) -> liquid::runtime::task<ApiResult> {
                if (list) {
                    if (auto refused = refuse_unless_reader(input, context)) {
                        co_return std::move(*refused);
                    }
                }
                json arguments = input;
                // The engine's spelling of the entity (`model`), where Lake publishes `model_id`.
                if (const auto entity =
                        std::ranges::find(entity_fields, engine, &EntityField::engine);
                    entity != entity_fields.end() && !entity->field.ends_with("_id")) {
                    const std::string field(entity->field);
                    if (arguments.contains(field + "_id")) {
                        arguments[field] = std::move(arguments[field + "_id"]);
                        arguments.erase(field + "_id");
                    }
                }
                json call{{"customer_id", arguments.at("customer_id")},
                          {"workspace_id", arguments.at("workspace_id")},
                          {"capability", engine}};
                arguments.erase("customer_id");
                arguments.erase("workspace_id");
                call["input"] = std::move(arguments);
                auto awaited = co_await call_workspace_capability(read, std::move(call), context);
                if (!awaited) {
                    co_return ApiResult::failure("store_error", awaited.error().message);
                }
                ApiResult result = std::move(*awaited);
                // workspace_query and workspace_act point below `/input`; here the fields are the
                // capability's own.
                if (!result.ok && result.error.path.starts_with("/input")) {
                    result.error.path.erase(0, 6);
                }
                co_return result;
            },
            .input_aliases = std::move(aliases),
            .granted_by_scope = read ? "workspace_query" : "workspace_act"});
    }
}

} // namespace lake::api
