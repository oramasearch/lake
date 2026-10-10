# MCP

`POST /mcp` is JSON-RPC 2.0 over HTTP, protocol version `2025-06-18`, the same shape Zetta
serves. Methods:

| Method | Behaviour |
|---|---|
| `initialize` | `{protocolVersion, capabilities: {tools: {listChanged: true}}, serverInfo: {name: "lake", version}}`, with an `Mcp-Session-Id` response header |
| `notifications/initialized` | accepted, `202`, no body |
| `ping` | `{}` |
| `tools/list` | every JSON registry entry as a tool: `name`, `title`, `description`, `inputSchema`, `outputSchema` (`lake::api::mcp_tools`) |
| `tools/call` | `params: {name, arguments}` → `Registry::call` with the request's bearer; result `{content: [{type: text, text}], structuredContent: <envelope>, isError}` |

The [shared envelope decision](README.md) defines both transports: `outputSchema` describes the
complete successful `structuredContent`, including `ok`, `value`, and `duration_ms`. Capability
errors carry the shared error envelope with `isError: true` over HTTP 200.

Unknown methods answer `-32601`; a body that is not JSON answers `400` with `-32700`; a request
without `"jsonrpc": "2.0"` answers `-32600`. `GET /mcp` and `DELETE /mcp` answer `405` with
`Allow: POST`: lake opens no standalone event stream and keeps no session to delete, and a `404`
would tell a Streamable HTTP client that its session expired.

## A changed tool list reaches connected clients (lake#441)

**What was observed.** On 2026-10-01 a connected MCP client listed 274 tools while the live
`tools/list` served 352, and its cached schemas lacked `workspace_id`, so
`workspace_status({customer_id})` failed with `workspace_id is required`. The client had listed
the tools once, from an older lake, and nothing told it the list had changed: `initialize`
declared `tools: {}` (no `listChanged`), and lake never sent `notifications/tools/list_changed`.
Within one process the registry is fixed and REST, OpenAPI and MCP already read the same entries,
so the divergence was a client holding a list across a deploy.

**What lake does now.**

- `initialize` declares `capabilities.tools.listChanged: true` and answers an `Mcp-Session-Id`:
  the hex SHA-256 of this build's tool list (`mcp_tool_list_digest`, both schema audiences), a dot,
  and 128 random bits. Lake keeps no session state; the identifier itself says which list the
  client was given.
- On every later request that carries an `Mcp-Session-Id` naming a different digest — lake was
  upgraded since the client initialized, on any replica — and whose client accepts
  `text/event-stream`, the response is Server-Sent Events: first
  `{"jsonrpc":"2.0","method":"notifications/tools/list_changed"}`, then the response the client
  asked for. The call itself still runs.
- `tools/list` never carries the notice, because its response is the fresh list: a client that
  refetches on the notice is not told again by that refetch. A client that accepts only
  `application/json` receives its plain response. During a rolling upgrade, replicas on the old
  and new builds can each send the notice; it stops once one build serves every request.
- A client that connected to a lake older than this change has no session identifier, so lake
  cannot tell what it holds. It must reconnect once; `/developer/mcp`, `llms.txt` and the lake
  skill tell people to reconnect after upgrades.

**One schema per capability.** `audience_input_schema` (registry) is the only input schema REST,
OpenAPI and MCP render. A caller sees the declared schema. An agent-task credential sees
`workspace_id` as optional, because `Registry::call` fills it from the task: `GET
/v1/openapi.json` for that credential and its `tools/list` are rendered from the same function
(they were two separate code paths before, and only OpenAPI relaxed the field). Registry
validation now also enforces `enum`, `minimum`, `maximum`, `exclusiveMinimum` and
`exclusiveMaximum`, so a value the published schema refuses — `run_pipeline`'s
`holdout_fraction` of `1.0` against its `maximum: 0.9` — is refused with `invalid_input` on REST
as well as MCP.

**How to verify.** `Mcp.ToolSchemasAreByteIdenticalToOpenApi` compares every tool's
`inputSchema`, `outputSchema`, title and description with its OpenAPI operation, byte for byte,
for both audiences. `Mcp.SessionsNameTheToolListTheyBeganOn` checks that an added capability
changes the digest. `HttpServer.McpSessionsLearnThatTheToolListChanged` drives the HTTP server:
the session header, no notice for a current session, the notice then the response for a stale
one, no notice on `tools/list`, plain JSON for a JSON-only client, and `405` for `GET` and
`DELETE`. `Registry.EnforcesEnumsAndNumericBoundsOnEveryTransport` covers the bounds.

Authorization is the REST bearer ([`auth.md`](auth.md)) on the same HTTP request; `tools/list`
needs none, `tools/call` on anything but `server_info` does. JSON operations have the same
capabilities, validation and errors as a REST client. Streaming liquid-sync registry entries are
omitted because MCP tool arguments cannot carry their binary request and response streams; they
remain documented in OpenAPI and [`sync.md`](sync.md).

Conflict tools use the same current Liquid model as REST. `list_conflicts` returns the conflict
kind, exact byte or node region, explanation, reasons, base/left/right values, and optional reviewed
proposal. `resolve_conflict` accepts `left`, `right`, `accept_proposal`, or `blob` with base64 bytes
and returns the conflict digest, selected value digest, and change digest. Both value schemas come from
the registry and appear under `outputSchema.properties.value` in `tools/list`.

```sh
curl -H 'content-type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' https://lake.oramadev.net/mcp
curl -H "Authorization: Bearer <token>" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_customers","arguments":{"limit":5}}}' \
  https://lake.oramadev.net/mcp
```

## Developer → MCP, llms.txt and the lake skill (lake#202)

**What.** A signed-in person connects an agent from one page of the web app, `/developer/mcp`
(sidebar Developer → MCP), which reads `mcp_setup`:

- **Create an MCP Key** mints a lake API token through `create_token`, the capability `/tokens`
  uses, with the scopes `mcp_setup` lists. *Every tool* scopes it `*`; *Read-only tools* scopes it to every
  read tool's name (`agent_docs::read_tool_names`; `workspace_query` counts as a read). The lifetime
  is 1 day, 7 days, or as long as the sign-in allows (at most 30 days): a token never outlives the
  sign-in that made it (lake#193). The key is shown once and revoked on `/tokens`.
- **Set Up**: copy-ready blocks with the viewer's key and lake's origin for Claude Code (`claude
  mcp add --transport http lake <origin>/mcp --header "Authorization: Bearer <key>"`), Cursor
  (`mcp.json` with `url` and `headers`), Claude Desktop (`mcp-remote` through `npx`), and plain
  JSON-RPC `curl` (`initialize`, `tools/list`). Each block has a **Copy** button: the page's one
  added script, `copy-buttons` (`navigator.clipboard.writeText` of the block's text).
- **After lake Upgrades**: reconnect or restart the MCP client (see lake#441 above).
- **Teach Your Agent**: *Download the lake skill* (`GET /skills/lake/SKILL.md`, **public** and linked
  from `/llms.txt` with a one-line install command; `/developer/mcp/skill` serves the same; `SKILL.md` as an
  attachment, for `~/.claude/skills/lake/`), `/llms.txt`, `/llms-full.txt`, and OpenAPI.
- **Keys**: every tool or read-only; by default as long as the sign-in (at most 30 days), 1 or 7 days,
  or **Never** (a standing key with a reason and named tools, where the space allows it, lake#234,
  lake#259).
- **Worked Flows**: load a dataset (`upload_dataset` through `workspace_act`), train a model and call
  it (`capture_intent` → `compile_intent` → `approve_plan` → `run_pipeline` → `get_run_report` →
  `expose_model` → `predict`), build and run a pipeline (`create_pipeline` → `edit_pipeline` →
  `get_pipeline` → `workspace_act` `approve_plan` → `run_pipeline`), stand up and connect a
  Postgres service (`list_service_kinds` → `create_service` → `list_services` →
  `service_connection` → `connect_service`), search a published index (`workspace_query`
  `list_indexes` → `workspace_act` `expose_index` → `search_index`).
- **What the Tools Do**: every MCP tool in product groups (Workspace, Services, Repositories,
  Organizations and access, Identity and sessions, Secrets, keys and certificates, Audit and
  transparency, Server), each with its title and read or write.

`GET /llms.txt` and `GET /llms-full.txt` are **public** (`text/plain`, cached 5 minutes):
what lake is, how to connect, every tool by group (llms-full adds each tool's description and input
schema), the flows and the rules an agent must keep (an edited pipeline is proposed again, a gate
failure runs nothing, search needs a published index, deleting destroys data).

**One source.** The page, both text files and the skill are generated by
`lake::api::agent_docs` (`include/lake/api/agent_docs.hpp`) from the capability registry, so they
list exactly the tools `tools/list` serves (the JSON capabilities; the liquid sync and ACME
protocol capabilities are not MCP tools). A new capability appears everywhere with no edit; a new
group or flow is an edit to `agent_docs.cpp`.

**How to verify.** `HttpResponseIsolation.AnMcpKeyWorksOverMcp` (llms.txt public; llms-full.txt
documents every `tools/list` tool; a key made with `create_token` and `mcp_setup`'s read-only
scopes lists tools, reads, and is refused a write; the skill downloads).
