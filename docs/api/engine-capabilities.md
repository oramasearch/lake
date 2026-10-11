# The workspace engine's capabilities, by name

lake#650. Everything a workspace's engine (its Zetta) does for a Lake caller is a named capability
in Lake's one registry: `/v1/<name>` over REST, its own OpenAPI operation with an input schema and a
description, and its own MCP tool. Before, these were reachable only through `workspace_query` and
`workspace_act`, whose `capability` field said nothing about the names it took or their inputs, so
an agent reading the MCP tool list saw two tools and could not find projects, training, models or
agents.

## What each named capability is

Every name in the allow-lists `workspace_reads` and `workspace_actions`
(`src/api/workspace_capabilities.cpp`) is served by one Lake capability
(`engine_capability_name`, `src/api/engine_capabilities.cpp`):

- **its own named capability**, registered from the engine's input schema; or
- **a Lake capability that already covered it** and does the same job, often more
  (`list_runs` filters and reads each run's stage, `get_pipeline` adds each step's status, the
  experiment capabilities say experiment where the engine says study).

A named capability is `workspace_query` or `workspace_act` by another name. It takes `customer_id`
and `workspace_id` and the engine's own fields at the top level, and runs
`call_workspace_capability`, the path those two take:

- **Policy:** `read` on the account for a read, `write` for an action; reads stay reads and
  actions stay actions, so OpenAPI's `x-lake-action` and MCP's `readOnlyHint` are what they were
  for the same call through `workspace_query` or `workspace_act`. `score` and `explain` are in both
  allow-lists and change nothing: they are reads. `chat_with_agent` is the engine's `chat`, which
  Lake authorizes as an action because it loads a model.
- **Credentials:** a token or agent credential whose scopes name `workspace_query` may call every
  named read, and one that names `workspace_act` every named action (`granted_by_scope`). An agent
  turn still acts only through the actions its credential names, by the engine's name or Lake's.
  An app's Zetta token, which may call `workspace_query`, may call the named reads.
- **Audit:** the event is the capability's name in the workspace's audit, with
  `workspace.query` or `workspace.act` as its kind and the engine's name as `capability`.
- **Caching and Lake's own rules:** the same workspace caller, so the same cached reads, and the
  same checks and answers: a project's name and `decision`, a dataset's names and
  `if_exists: "rename"`, a Python agent's `definition`, the `call` that serves a published model or
  index, `<database>/<name>` dataset ids.
- **Refusals** point at the caller's field (`/name`), not below `/input`.

## Where the schemas come from

From the engine, never written by hand. `tools/api/vendor-engine-openapi.py` reads Zetta's
generated, drift-checked `schemas/openapi.json` (the Zetta source tree Lake builds against,
`LAKE_ZETTA_SOURCE_DIR`) or a document a workspace engine serves
(`GET /workspaces/<customer_id>/zetta/v1/openapi.json`, the filtered copy agent-runtime reads) and
writes the allow-listed operations to `src/api/engine_openapi.json`, which the build embeds
(`cmake/embed_engine_openapi.cmake`). Lake then:

- names the entity as Lake names it (docs/api/naming.md): `model_id` and `index_id` where the
  engine says `model` and `index`, with the engine's spelling still accepted as a deprecated alias;
  `run_id` and `job_id` as the engine has them, `run` and `job` accepted too;
- adds `customer_id` and `workspace_id`, and its own fields: `decision` on `create_project`,
  `if_exists` on `create_dataset_from_query`;
- takes the engine's description, without its issue references and permission notes, with the
  names Lake gives renamed capabilities, and adds what Lake does beside it.

A capability the engine publishes no schema for is still registered, with a description that says
so and its fields passed as sent; there is none today. Refresh after a Zetta change to the
allow-listed capabilities:

```sh
tools/api/vendor-engine-openapi.py --zetta-source "$LAKE_ZETTA_SOURCE_DIR"
tools/api/vendor-engine-openapi.py --zetta-source "$LAKE_ZETTA_SOURCE_DIR" --check
```

Reads are `GET` with the input in the query string and also take a JSON body on `POST`
(`post_alias`), since some take nested input (`validate_experiment`'s parameter ranges).

## Names

The engine's name is kept unless it collides with a Lake capability that does something else, or
says too little on its own. Lake says experiment where the engine says study (lake#561), and
pipeline where it says graph or plan.

| Engine | Lake | Why |
|---|---|---|
| `server_info` | `workspace_engine_info` | `server_info` is Lake's own |
| `search` | `search_workspace_index` | `search` is Lake's search of everything you can open; `search_index` searches a published index, this one any index |
| `get_experiment` | `get_holdout_experiment` | a solution's holdout experiment; `get_experiment` is Lake's training experiment |
| `create_solution` | `compose_solution` | Lake's `create_solution` keeps a workspace's own solution; this composes a catalogue solution's governed foundation into a project |
| `validate_study` | `validate_experiment` | experiment, not study |
| `gate` | `check_pipeline` | runs the correctness checks on a pipeline and changes nothing |
| `run` | `start_run` | `run_pipeline` checks and runs in one call; this runs a checked pipeline with every runner setting |
| `deploy` | `deploy_pipeline` | places a pipeline on a target |
| `approve_plan` | `approve_pipeline` | approves a proposed pipeline |
| `save_graph` | `save_pipeline` | saves a whole pipeline; `edit_pipeline` applies edits |
| `chat` | `chat_with_agent` | `chat` alone does not say with what |

`workspace_query` and `workspace_act` stay for compatibility and still take the engine's names.
Their `capability` field carries an `enum` of the names each allows, so OpenAPI and MCP list them.

## Every engine capability and the Lake capability that serves it

The groups are the ones `llms.txt` and `llms-full.txt` list them under.

### Projects and pipelines

| Lake capability | Engine capability | Permission |
|---|---|---|
| `list_projects` | same | read |
| `get_project` | same | read |
| `list_methodologies` | same | read |
| `create_project` | same | write |
| `capture_intent` | same | write |
| `compile_intent` | same | write |
| `approve_pipeline` | `approve_plan` | write |
| `check_pipeline` | `gate` | write |
| `delete_project` | same | write |
| `save_pipeline` | `save_graph` | write |
| `deploy_pipeline` | `deploy` | write |
| `import_n8n` | same | write |
| `compile_n8n` | same | write |
| `compile_python` | same | write |

### Training and runs

| Lake capability | Engine capability | Permission |
|---|---|---|
| `get_run` | same | read |
| `get_run_report` | same | read |
| `list_jobs` | same | read |
| `get_job` | same | read |
| `start_run` | `run` | write |

### Experiments

| Lake capability | Engine capability | Permission |
|---|---|---|
| `validate_experiment` | `validate_study` | read |

### Models

| Lake capability | Engine capability | Permission |
|---|---|---|
| `score` | same | read |
| `explain` | same | read |
| `list_models` | same | read |
| `model_health` | same | read |
| `expose_model` | same | write |
| `unexpose_model` | same | write |

### Search indexes

| Lake capability | Engine capability | Permission |
|---|---|---|
| `list_indexes` | same | read |
| `get_index` | same | read |
| `search_workspace_index` | `search` | read |
| `create_index` | same | write |
| `expose_index` | same | write |
| `unexpose_index` | same | write |

### Datasets

| Lake capability | Engine capability | Permission |
|---|---|---|
| `list_dataset_generators` | same | read |
| `analyze_dataset` | same | read |
| `plan_features` | same | read |
| `create_dataset_from_query` | same | write |
| `upload_dataset` | same | write |
| `promote_dataset` | same | write |
| `generate_dataset` | same | write |

### Connections

| Lake capability | Engine capability | Permission |
|---|---|---|
| `list_connections` | same | read |
| `list_source_tables` | same | read |
| `create_connection` | same | write |
| `delete_connection` | same | write |

### Agents

| Lake capability | Engine capability | Permission |
|---|---|---|
| `list_agents` | same | read |
| `get_agent` | same | read |
| `agent_training_status` | same | read |
| `create_agent` | same | write |
| `update_agent` | same | write |
| `delete_agent` | same | write |
| `set_training_schedule` | same | write |
| `agent_run` | same | write |
| `chat_with_agent` | `chat` | write |

### Solutions

| Lake capability | Engine capability | Permission |
|---|---|---|
| `compose_solution` | `create_solution` | write |
| `run_solution` | same | write |

### Decisions

| Lake capability | Engine capability | Permission |
|---|---|---|
| `get_policy` | same | read |
| `get_holdout_experiment` | `get_experiment` | read |
| `list_ledger` | same | read |
| `get_monitor` | same | read |
| `get_roi_readout` | same | read |
| `update_policy` | same | write |
| `decide` | same | write |
| `approve_decision` | same | write |
| `record_outcome` | same | write |

### Engine

| Lake capability | Engine capability | Permission |
|---|---|---|
| `workspace_engine_info` | `server_info` | read |
| `get_trace` | same | read |
| `list_traces` | same | read |

### Covered by a Lake capability that already existed

| Engine capability | Lake capability | Permission |
|---|---|---|
| `get_run_metrics` | `get_run_metrics` | read |
| `compare_runs` | `compare_runs` | read |
| `get_study` | `get_experiment` | read |
| `list_studies` | `list_experiments` | read |
| `list_trials` | `list_experiment_trials` | read |
| `get_study_analysis` | `get_experiment_analysis` | read |
| `browse_topic` | `browse_stream` | read |
| `get_graph` | `get_pipeline` | read |
| `list_datasets` | `list_datasets` | read |
| `get_dataset` | `get_dataset` | read |
| `list_runs` | `list_runs` | read |
| `get_model` | `get_model` | read |
| `list_topics` | `list_streams` | read |
| `receive_events` | `browse_stream` | read |
| `list_solutions` | `list_solutions` | read |
| `get_solution` | `get_solution` | read |
| `create_study` | `start_experiment` | write |
| `stop_study` | `stop_experiment` | write |
| `stop_trial` | `stop_experiment_trial` | write |
| `promote_trial` | `promote_trial` | write |

## Tests

`EngineCapabilities.EveryAllowListedNameHasANamedCapability` fails when a name in
`workspace_reads` or `workspace_actions` has no named capability, or its capability is not
registered, is not a read or an action as the allow-list says, or is not grouped;
`EngineCapabilities.McpAndOpenApiDescribeTheSameCapabilities` fails when an MCP tool and its
OpenAPI operation differ in name or input schema; and
`EngineCapabilities.ANamedCallReachesTheWorkspaceEngineAsWorkspaceActDoes` calls them through the
registry against a fake workspace engine (`tests/test_workspaces.cpp`).

The web app still calls `workspace_query` and `workspace_act`; it can move to the named
capabilities page by page.
