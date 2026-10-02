#include "lake/api/agent_docs.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>

#include "lake/api/mcp.hpp"

namespace lake::api::agent_docs {
namespace {

bool contains_any(std::string_view name, std::initializer_list<std::string_view> parts) {
    return std::ranges::any_of(
        parts, [&](std::string_view part) { return name.find(part) != std::string_view::npos; });
}

// workspace_query is registered as an action (it runs through the workspace's allow-list) but it
// only reads; a read-only key includes it.
bool writes(const Capability& capability) {
    return capability.kind == CapabilityKind::Action && capability.name != "workspace_query";
}

struct GroupRule {
    std::string_view title, summary;
    bool (*matches)(std::string_view name);
};

// First match wins, so the narrow groups come before Repositories, which takes the rest.
constexpr std::array group_rules{
    GroupRule{"Experiments",
              "Read training studies, run metrics, comparisons and bounded live events in a space. "
              "These tools are distinct from the solutions holdout get_experiment. Start and stop "
              "require write permission and the person's confirmation; promote_trial requires "
              "space administration and confirmation. Curves are suppressed for differential privacy.",
              [](std::string_view n) {
                  return n == "list_experiments" || n == "get_experiment_study" ||
                         n == "list_experiment_trials" || n == "get_experiment_analysis" ||
                         n == "tail_experiment_events" || n == "stop_experiment_trial" ||
                         n == "get_run_metrics" ||
                         n == "compare_runs" || n == "start_study" || n == "stop_study" ||
                         n == "promote_trial";
              }},
    GroupRule{"Streams",
              "Ask Orama and ApplicationAgent may freely read list_streams, get_stream and "
              "browse_stream. Stream writes require the person's confirmation; deletes require "
              "space administration. System streams are browse-only. browse_stream never "
              "consumes messages; use wait_ms for bounded live reads over REST or MCP.",
              [](std::string_view n) {
                  return n.find("stream") != std::string_view::npos;
              }},
    GroupRule{"Agents",
              "The Agent Library of specialized agents Orama ships, and the agents you build in "
              "Train > Agents: create them through workspace_act, talk to any of them with "
              "agent_turn, run Python agents with launch_agent and give them cron schedules with "
              "schedule_agent (each its own Kubernetes CronJob, at most once every five minutes). "
              "A Python agent's definition is {source: zetta, instructions, entrypoint, files: "
              "[{path, contents}]}; lake refuses any other field. Runs and schedules belong to "
              "the workspace they were started in.",
              [](std::string_view n) {
                  return n == "list_agent_library" || n == "launch_agent" ||
                         n == "list_agent_launches" || n == "stop_agent_launch" ||
                         n == "schedule_agent" || n == "list_agent_schedules" ||
                         n == "unschedule_agent" || n == "agent_turn" ||
                         n == "create_agent_from_proposal" ||
                         n == "create_projects_from_opportunities" || n == "start_solution" ||
                         n == "confirm_solution" || n == "get_ask_conversation";
              }},
    GroupRule{"Workspace: Design, Train and Ship",
              "Your Zetta workspace: projects and pipelines, data, runs, models and search "
              "indexes. workspace_query and workspace_act reach every allowed Zetta capability.",
              [](std::string_view n) {
                  return contains_any(n, {"pipeline"}) || n.starts_with("workspace_") ||
                         n == "start_workspace" || n == "delete_workspace" || n == "search_index" ||
                         n == "predict";
              }},
    GroupRule{"Services",
              "Databases, streams and engines lake runs for you (Postgres, Zetta, Kafka, "
              "ClickHouse, TiDB, Cassandra, Valkey, SQLite, DuckDB, Polars), their connections, "
              "and connecting one to your workspace.",
              [](std::string_view n) { return contains_any(n, {"service"}); }},
    GroupRule{"Storage",
              "Buckets, filesystems, network filesystems and volumes lake provisions for you "
              "(a cluster volume today), and how to reach them.",
              [](std::string_view n) {
                  // Exact names: restore_drive_version and others also contain "store".
                  return n == "list_store_providers" || n == "list_stores" || n == "create_store" ||
                         n == "delete_store" || n == "store_connection";
              }},
    GroupRule{"Audit and transparency",
              "The signed audit log, the transparency log and its proofs, timestamps and TUF "
              "metadata.",
              [](std::string_view n) {
                  return contains_any(
                      n, {"audit", "transparency", "timestamp", "tuf", "replicat", "checkpoint"});
              }},
    GroupRule{"Secrets, keys and certificates",
              "Static and dynamic secrets, encryption keys, signing, and the customer PKI.",
              [](std::string_view n) {
                  return contains_any(n,
                                      {"secret",
                                       "encrypt",
                                       "decrypt",
                                       "rewrap",
                                       "hmac",
                                       "_key",
                                       "pki",
                                       "certificate",
                                       "svid",
                                       "kek"}) ||
                         n == "sign" || n == "verify";
              }},
    GroupRule{"Identity and sessions",
              "Sign-in, passkeys, multi-factor authentication, sessions, tokens, devices and "
              "recovery.",
              [](std::string_view n) {
                  return contains_any(n,
                                      {"sign_in",
                                       "sign_up",
                                       "passkey",
                                       "mfa",
                                       "session",
                                       "token",
                                       "device",
                                       "recover",
                                       "identity",
                                       "oauth",
                                       "attestation",
                                       "offline",
                                       "agent",
                                       "cached_read",
                                       "jwt"});
              }},
    GroupRule{"Organizations and access",
              "Organizations, teams, members, invitations, grants, customers, SSO and SCIM, "
              "applications, usage and quotas.",
              [](std::string_view n) {
                  return contains_any(n,
                                      {"organization",
                                       "team",
                                       "member",
                                       "invitation",
                                       "grant",
                                       "effective_access",
                                       "customer",
                                       "user",
                                       "sso",
                                       "scim",
                                       "federation",
                                       "application",
                                       "quota",
                                       "metering",
                                       "role",
                                       "admin"});
              }},
    GroupRule{"Server",
              "What this lake is.",
              [](std::string_view n) { return n == "server_info" || n == "mcp"; }},
    GroupRule{"Repositories",
              "Repositories and their files, changes, reviews, issues, releases, webhooks, "
              "artifacts, drive, models and datasets, environments and search.",
              [](std::string_view) { return true; }},
};

// The order groups are shown in.
constexpr std::array<std::string_view, 12> group_order{"Workspace: Design, Train and Ship",
                                                       "Experiments",
                                                       "Streams",
                                                       "Agents",
                                                       "Services",
                                                       "Storage",
                                                       "Repositories",
                                                       "Organizations and access",
                                                       "Identity and sessions",
                                                       "Secrets, keys and certificates",
                                                       "Audit and transparency",
                                                       "Server"};

std::string required_fields(const Capability& capability) {
    std::string out;
    for (const auto& field : capability.input_schema.value("required", nlohmann::json::array())) {
        if (field.is_string()) {
            out += (out.empty() ? "" : ", ") + field.get<std::string>();
        }
    }
    return out.empty() ? "none" : out;
}

std::string mcp_endpoint(std::string_view origin) {
    return std::string(origin) + "/mcp";
}

std::string how_to_connect(std::string_view origin) {
    return std::format(
        "lake's MCP server is `{0}` (Streamable HTTP, JSON-RPC 2.0, protocol {1}: `initialize`, "
        "`ping`, `tools/list`, `tools/call`). Authenticate with `Authorization: Bearer <key>`: a "
        "lake API token. Create one on {2}/developer/mcp (or {2}/tokens). A token lasts as long as "
        "the sign-in that made it (at most 30 days) unless it is a standing key that never "
        "expires, which a space must allow and which names its tools and a reason.\n\n"
        "- Claude Code: `claude mcp add --transport http lake {0} --header \"Authorization: "
        "Bearer $LAKE_KEY\"`\n"
        "- REST: every tool is also `{2}/v1/<tool>` (a read is GET with a query string, an "
        "action is POST with a JSON body); the schema is `{2}/v1/openapi.json`.\n"
        "- Answers are `{{\"ok\": true, \"value\": …}}` or `{{\"ok\": false, \"error\": "
        "{{\"code\", \"message\"}}}}`.\n"
        "- After lake is upgraded, reconnect your MCP client to lake (or restart it). "
        "lake declares `tools.listChanged` and sends `notifications/tools/list_changed` to a "
        "session that began on an older tool list, but a client that connected before an upgrade "
        "or ignores the notice keeps the tools and schemas it first listed.\n",
        mcp_endpoint(origin),
        mcp_protocol_version,
        origin);
}

std::string flows_markdown(const Registry& registry) {
    std::string out;
    for (const Flow& flow : flows()) {
        out += std::format("### {}\n\n{}\n\n", flow.title, flow.summary);
        int number = 1;
        for (const FlowStep& step : flow.steps) {
            const auto capability = registry.find(step.tool);
            out += std::format("{}. `{}`{} — {}\n",
                               number++,
                               step.tool,
                               capability ? " (requires: " + required_fields(*capability) + ")"
                                          : std::string(),
                               step.note);
        }
        out += "\n";
    }
    return out;
}

constexpr std::string_view rules =
    "- Every Design, Train and Ship data-plane tool takes `customer_id` and an explicit "
    "`workspace_id` from `list_workspaces`. A workspace ID is not a service ID. Create one with "
    "`create_workspace` (name and environment), then wait for `workspace_status` to report ready.\n"
    "- An edited pipeline is **proposed** again: approve it (`workspace_act`, capability "
    "`approve_plan`) before `run_pipeline`.\n"
    "- `run_pipeline` runs Zetta's checks first; a failing gate answers `{gate}` and runs "
    "nothing. Read every check's reason.\n"
    "- A service is usable when `list_services` reports it `Ready`; `service_connection` "
    "returns its credentials. Never print them.\n"
    "- `search_index` works only on a **published** index.\n"
    "- `workspace_sql` runs one read-only `SELECT` or `WITH`.\n"
    "- Deleting a workspace, a service, a store or a repository destroys its data: ask the "
    "person first.\n";

} // namespace

std::vector<Group> tool_groups(const Registry& registry) {
    std::vector<Group> groups;
    for (const std::string_view title : group_order) {
        const auto rule = std::ranges::find(group_rules, title, &GroupRule::title);
        groups.push_back({std::string(rule->title), std::string(rule->summary), {}});
    }
    for (const auto& capability : registry.list()) {
        if (capability->transport != CapabilityTransport::Json) {
            continue;
        }
        const auto rule = std::ranges::find_if(
            group_rules, [&](const GroupRule& r) { return r.matches(capability->name); });
        const auto group = std::ranges::find(groups, rule->title, &Group::title);
        group->tools.push_back({capability->name,
                                capability->title,
                                capability->description,
                                writes(*capability),
                                capability->requires_auth});
    }
    std::erase_if(groups, [](const Group& group) { return group.tools.empty(); });
    return groups;
}

std::vector<std::string> read_tool_names(const Registry& registry) {
    std::vector<std::string> names;
    for (const auto& capability : registry.list()) {
        if (capability->transport == CapabilityTransport::Json && !writes(*capability)) {
            names.push_back(capability->name);
        }
    }
    return names;
}

const std::vector<Flow>& flows() {
    static const std::vector<Flow> all{
        {"Build and run a pipeline",
         "A pipeline is a Zetta execution graph of steps (connect, clean, model, index, serve, "
         "act) joined by edges.",
         {{"create_pipeline", "`from: \"empty\"` in a project; answers the new graph's name"},
          {"edit_pipeline",
           "one edit per call: `op: \"add_step\"` with `step`, `kind`, `name`; `op: \"add_edge\"` "
           "with `from`, `to`; also rename_step, kind_step, remove_step, remove_edge, rename"},
          {"get_pipeline", "read the graph back: steps, edges, approval"},
          {"workspace_act",
           "`capability: \"approve_plan\"`, `input: {project, graph}`: an edited pipeline must be "
           "approved again"},
          {"run_pipeline", "runs the checks, then the pipeline; answers {gate, run}"}}},
        {"Load a dataset",
         "A dataset is a table in your workspace. Upload a CSV or JSONL file's text (up to 16 MiB).",
         {{"create_workspace",
           "once (a `name` and an `environment`); its `id` is the `workspace_id` every later call "
           "names. Poll `workspace_status` until it is ready"},
          {"workspace_act",
           "`capability: \"upload_dataset\"`, `input: {table_name, format: \"csv\"|\"jsonl\", "
           "content}`; answers the rows loaded and the dataset's `id`. A name is letters, digits "
           "and underscores; a taken name answers `conflict` unless `if_exists: \"rename\"`; a "
           "CSV row whose field count differs from the header is refused with its line"},
          {"workspace_query", "`capability: \"get_dataset\"`, `input: {dataset}` (the `id` "
           "`<database>/<name>` from `list_datasets`): its columns"},
          {"workspace_sql", "`statement: \"SELECT … LIMIT 20\"` to look at it"}}},
        {"Train a model and call it",
         "Train from a dataset, read the report, publish the model and score new rows.",
         {{"workspace_act",
           "`capability: \"create_project\"`, `input: {name, description, intent}`"},
          {"workspace_act",
           "`capability: \"capture_intent\"`, `input: {project, text: <what to predict>}`, then "
           "`capability: \"compile_intent\"`, `input: {project, input_id}`: a pipeline built from "
           "the sentence"},
          {"workspace_act", "`capability: \"approve_plan\"`, `input: {project, graph}`"},
          {"run_pipeline",
           "`dataset_path: \"datasets.<name>\"`, `holdout_fraction: 0.2`, `model_name`; answers "
           "{gate, run}"},
          {"workspace_query", "`capability: \"get_run_report\"`, `input: {run_id}` once it finished"},
          {"workspace_act", "`capability: \"expose_model\"`, `input: {model: <id>}` to publish it"},
          {"predict", "`model`, `rows`: scores rows through the published model"}}},
        {"Stand up a Postgres service and connect it",
         "A service is a database, stream or engine lake runs for your space.",
         {{"list_service_kinds", "the kinds, their expected start times, which are preview"},
          {"create_service", "`kind: \"postgres\"`, a `name`; optional `instances`, `storage_gi`"},
          {"list_services", "poll until the service is `Ready`"},
          {"service_connection", "its endpoint and credentials (keep them secret)"},
          {"connect_service", "make it a Train → Data connection of your workspace"}}},
        {"Create a volume",
         "A store is a bucket, filesystem, network filesystem or volume lake provisions for your "
         "space; today a cluster volume.",
         {{"list_store_providers", "every type and provider, and which are available"},
          {"create_store",
           "`type: \"volume\"`, `provider: \"cluster\"`, a `name`; optional `size_gi`"},
          {"list_stores", "poll until the store is `Ready`"},
          {"store_connection", "the claim, namespace, size and storage class to mount"}}},
        {"Search a published index",
         "A search index is BM25F over a dataset; outside callers can search it once it is "
         "published.",
         {{"workspace_query", "`capability: \"list_indexes\"` to find the index id"},
          {"workspace_act",
           "`capability: \"expose_index\"` to publish it (`unexpose_index` to stop)"},
          {"search_index", "`index`, `query`; optional `field`, `top_k`"}}},
    };
    return all;
}

std::string llms_txt(const Registry& registry, std::string_view origin) {
    std::string out = std::format(
        "# lake\n\n"
        "> lake is Orama's hosted platform: private liquid repositories, and Zetta workspaces in "
        "each customer space (a call names one with `workspace_id`) where you Design (projects and pipelines), Train (data, runs) and Ship "
        "(models, search indexes, services). Everything a person can do on the site is a "
        "capability in one registry, served as REST, OpenAPI and MCP.\n\n"
        "## Connect\n\n{0}\n"
        "## The lake skill for Claude\n\n"
        "Download [SKILL.md]({1}/skills/lake/SKILL.md) (public, no key needed) and install it:\n\n"
        "```sh\nmkdir -p ~/.claude/skills/lake && curl -fsSL {1}/skills/lake/SKILL.md -o "
        "~/.claude/skills/lake/SKILL.md\n```\n\n"
        "It says how to connect, which tools to use for which task, the worked flows and the "
        "rules.\n\n"
        "## Learn more\n\n"
        "- [Every tool with its input schema]({1}/llms-full.txt)\n"
        "- [OpenAPI]({1}/v1/openapi.json)\n"
        "- [MCP setup and keys]({1}/developer/mcp)\n\n"
        "## Tools\n\n",
        how_to_connect(origin),
        origin);
    for (const Group& group : tool_groups(registry)) {
        out += std::format("### {}\n\n{}\n\n", group.title, group.summary);
        for (const Tool& tool : group.tools) {
            out += std::format(
                "- `{}` ({}): {}\n", tool.name, tool.writes ? "write" : "read", tool.title);
        }
        out += "\n";
    }
    out += "## Worked flows\n\n" + flows_markdown(registry);
    out += "## Rules\n\n" + std::string(rules);
    return out;
}

std::string llms_full_txt(const Registry& registry, std::string_view origin) {
    std::string out = std::format(
        "# lake — every tool\n\n{}\nThe lake skill for Claude: {}/skills/lake/SKILL.md (public).\n\n",
        how_to_connect(origin),
        origin);
    for (const Group& group : tool_groups(registry)) {
        out += std::format("## {}\n\n{}\n\n", group.title, group.summary);
        for (const Tool& tool : group.tools) {
            const auto capability = registry.find(tool.name);
            out += std::format("### `{}` — {}\n\n{} {}\n\nInput schema:\n\n```json\n{}\n```\n\n",
                               tool.name,
                               tool.title,
                               tool.writes ? "**Write.**" : "**Read.**",
                               tool.description,
                               capability ? capability->input_schema.dump(2) : "{}");
        }
    }
    out += "## Worked flows\n\n" + flows_markdown(registry);
    out += "## Rules\n\n" + std::string(rules);
    return out;
}

std::string skill_md(const Registry& registry, std::string_view origin) {
    std::string out = std::format(
        "---\n"
        "name: lake\n"
        "description: Work in lake ({0}) through its MCP server — Design, Train and Ship in a "
        "Zetta workspace (projects, pipelines, data, runs, models, search indexes), managed "
        "services (Postgres, Kafka, ClickHouse, …), and repositories. Use when asked to build or "
        "run a pipeline, stand up or connect a database service, search an index, or do anything "
        "on lake.\n"
        "---\n\n"
        "# lake\n\n"
        "## Connect\n\n{1}\n"
        "Check the connection with `server_info`, then `tools/list`. Full reference: "
        "{0}/llms.txt and {0}/llms-full.txt (every tool's input schema). The latest copy of this "
        "skill is always at {0}/skills/lake/SKILL.md.\n\n"
        "## Which tools for which task\n\n",
        origin,
        how_to_connect(origin));
    for (const Group& group : tool_groups(registry)) {
        if (group.title == "Server") {
            continue;
        }
        std::string sample;
        int shown = 0;
        for (const Tool& tool : group.tools) {
            if (shown++ == 8) {
                sample += ", …";
                break;
            }
            sample += (sample.empty() ? "`" : ", `") + tool.name + "`";
        }
        out += std::format(
            "- **{}** ({} tools): {} {}\n", group.title, group.tools.size(), group.summary, sample);
    }
    out += "\n## Flows\n\n" + flows_markdown(registry);
    out += "## Rules\n\n" + std::string(rules);
    return out;
}

} // namespace lake::api::agent_docs
