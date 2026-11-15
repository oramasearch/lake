# Zetta parity: every Zetta capability in lake

**Status: plan, 2026-09-26.** The pipeline is a central object in lake and Zetta: it is fully
visible, displayable and changeable, and lake has full feature parity with Zetta. Tracking:
https://github.com/oramasearch/lake/issues/185.

lake delegates Design, Train and Ship to the customer's own Zetta cluster, its workspace
([`../api/workspaces.md`](../api/workspaces.md)). Parity means every capability Zetta offers a
person is reachable from lake: as a page or a form, and through lake's registry
(`workspace_query` / `workspace_act`), so REST, MCP and the UI stay one surface. Zetta's own list
is its OpenAPI: 80 capabilities at zetta `dev` `f84ab6f`.

## The pipeline is the central object

A **pipeline** is a Zetta execution graph: typed steps (`connect`, `clean`, `model`, `index`,
`serve`, `act`; zetta `include/zetta/ir/step_kind.hpp`) joined by directed edges `{from, to,
kind}`, owned by a project, with an approval state (`proposed` / `approved`). Its life:

```
intent / n8n / Python ──compile──▶ proposed ──approve──▶ approved ──gate──▶ run ──▶ report, traces
        ▲                              │                         └──deploy──▶ deployment
        └──────── save_graph (edit) ◀──┘   an edit makes it proposed again
```

The pipeline page (`/design/pipeline?project=&graph=`) shows all of that in one place:

- **See it.** The graph drawn as a server-rendered SVG, steps laid out in stage lanes
  (connect → clean → model → index → serve → act), edges as arrows, each step coloured by its
  latest run's outcome. No JavaScript: the page stays within lake's page rules.
- **Inspect it.** Select a step for its id, kind, name, incoming and outgoing edges, and what the
  last run did with it.
- **Change it.** Rename the pipeline; add, rename, re-kind or remove a step; add or remove an
  edge. Each change is one form posting the whole graph to `save_graph` (Zetta replaces it
  atomically, replicates and audits it), after lake checks that ids are unique, edges name existing
  steps, and the graph has no cycle. A saved change leaves the plan `proposed`, so it is approved
  again before it runs.
- **Govern it.** Approve (`approve_plan`); check (`gate`), with every check's name, status and
  reason shown, not just a verdict; run (`run`); deploy (`deploy`, with its gate).
- **Follow it.** Its runs, each with status, the report rendered as tables (dataset features,
  drivers and their weights, the ranked at-risk list) rather than raw JSON, and the run's trace
  (`get_trace`) as a span waterfall.
- **Create it** from a description (`capture_intent` → `compile_intent`), from an n8n workflow
  (`import_n8n`, `compile_n8n`), from simulated Python (`compile_python`), or from nothing (an
  empty pipeline, built step by step).

## Capability map

Each Zetta capability, where it lives in lake, and the phase that delivers it. ✅ is shipped today.

| Zetta capability | lake surface | Phase |
|---|---|---|
| `list_projects`, `get_project`, `create_project`, `delete_project` | Design → Projects, project page | ✅ |
| `capture_intent`, `compile_intent` | project page, *Build a pipeline* | ✅ |
| `approve_plan`, `gate`, `run` | pipeline row buttons | ✅ (becomes the pipeline page, 1) |
| `get_graph` | **pipeline page: the graph, steps, edges** | ✅ 1 |
| `save_graph` | **pipeline page: every edit**, through lake's `edit_pipeline` | ✅ 1 |
| `deploy` | pipeline page, *Deploy*, with its gate checks | ✅ 1 |
| `compile_n8n`, `import_n8n` | project page, *Import from n8n* | ✅ 1 |
| `compile_python` | project page, *Build from Python* | ✅ 1 |
| `list_runs`, `get_run`, `get_run_report` | Train → Runs, run page (report as tables, lineage, outputs, log) | ✅ |
| `get_trace`, `list_traces` | run page *Trace* (spans naming the run); Train → Traces page in 5 | ✅ 1 |
| `list_connections`, `create_connection`, `delete_connection` | Train → Data | ✅ |
| `create_dataset_from_query`, `upload_dataset` | Train → Data | ✅ |
| `list_source_tables` | Train → Data, *Browse tables* | ✅ 2 |
| `list_datasets`, `get_dataset` | Train → Data, dataset page (schema, partitions, sample) | ✅ 2 |
| `add_dataset` | Train → Data (server-side file path; operators only) | 2 |
| `list_dataset_generators`, `generate_dataset` | Train → Data, *Generate a dataset* | ✅ 2 |
| `promote_dataset` | dataset page, *Keep durable* | ✅ 2 |
| `sql` | Train → Query (lake's `workspace_sql`: one read-only SELECT/WITH) | ✅ 2 |
| `list_models`, `get_model`, `model_health` | Ship → Models, model page | ✅ list; page 3 |
| `expose_model`, `unexpose_model` | Ship → Models | ✅ |
| `score`, `explain` | model page, *Score* and *Explain a row* | 3 |
| `predict` | model page, *Try the endpoint* | 3 |
| `list_indexes`, `get_index`, `create_index` | Ship → Search indexes, index page, *New index* | ✅ (#187) |
| `expose_index`, `unexpose_index` | index page, *Publish* / *Unpublish* | ✅ (#187) |
| `search`, `serve_search` | index page, *Try a search*; outside callers through lake's `search_index` with a key scoped to it (Zetta's `serve_search` stays inside the cluster) | ✅ (#187) |
| `list_agents`, `get_agent`, `create_agent`, `update_agent`, `delete_agent` | Design → Agents | 4 |
| `chat` | agent page, conversation | 4 |
| `agent_run` | agent page, *Build and run a pipeline* | 4 |
| `agent_training_status`, `set_training_schedule` | agent page, *Training* | 4 |
| `submit_feedback`, `propose_improvement` | run and agent pages | 4 |
| `list_consent_requests`, `approve_consent`, `deny_consent` | Organization → Approvals | 5 |
| `list_jobs`, `get_job` | Train → Jobs | 5 |
| `list_specs`, `get_spec` | Developer → Workspace API (every capability's schema) | 5 |
| `testsuite_list`, `testsuite_run` | Developer → Test suite | 5 |
| `create_workspace_profile`, `get_workspace_profile`, `update_workspace_profile` | Organization → Workspace profile | 5 |
| `create_plan`, `get_plan`, `update_plan` | Organization → Usage (workspace plan and quotas) | 5 |
| `server_info`, `versions` | Services, the workspace row's details | 5 |
| `add_nodes`, `cloud_inspect`, `detect_provisioners`, `deployment_profile` | **not exposed.** Infrastructure belongs to lake's services and the operator: a workspace's size is its service's `instances`, set from the Services page | — |

`sql` is exposed read-only because the workspace is the customer's own cluster: a
`SELECT` over its data reveals nothing another customer owns. Its writes, users and administrative
statements stay out of reach.

## Rules every phase keeps

- **One registry.** Each capability joins the `workspace_query` / `workspace_act` allow-lists
  (`src/api/workspace_capabilities.cpp`), so it is authorized on the customer and audited before it
  reaches Zetta, and REST and MCP get it with the page.
- **Pure renderers, forms, no client JavaScript** (lake `CLAUDE.md`). The graph is SVG laid out on
  the server.
- **Every page is proven in the browser** (`tools/e2e/README.md`); the walkthrough
  grows a step per phase.
- **Zetta's schema is the contract.** Field names come from Zetta's OpenAPI at the pinned commit;
  when Zetta changes, the pin moves and the page follows.

## Tracking

**Phases 3–5 are paused (Steve, 2026-09-26): do not implement them until the pause is lifted.**

| Phase | Issue |
|---|---|
| Tracking | https://github.com/oramasearch/lake/issues/185 |
| 1 — the pipeline page | https://github.com/oramasearch/lake/issues/180 |
| 2 — data | https://github.com/oramasearch/lake/issues/181 |
| 3 — models and search | https://github.com/oramasearch/lake/issues/182 |
| 4 — agents | https://github.com/oramasearch/lake/issues/183 |
| 5 — operations | https://github.com/oramasearch/lake/issues/184 |

Each closes with the PR that ships it and the browser proof.
