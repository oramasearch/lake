# MCP

`POST /mcp` is JSON-RPC 2.0 over HTTP, protocol version `2025-06-18`, the same shape Zetta
serves. Methods:

| Method | Behaviour |
|---|---|
| `initialize` | `{protocolVersion, capabilities: {tools: {}}, serverInfo: {name: "lake", version}}` |
| `notifications/initialized` | accepted, `202`, no body |
| `ping` | `{}` |
| `tools/list` | every JSON registry entry as a tool: `name`, `title`, `description`, `inputSchema`, `outputSchema` |
| `tools/call` | `params: {name, arguments}` → `Registry::call` with the request's bearer; result `{content: [{type: text, text}], structuredContent: <envelope>, isError}` |

The [shared envelope decision](README.md) defines both transports: `outputSchema` describes the
complete successful `structuredContent`, including `ok`, `value`, and `duration_ms`. Capability
errors carry the shared error envelope with `isError: true` over HTTP 200.

Unknown methods answer `-32601`; a body that is not JSON answers `400` with `-32700`; a request
without `"jsonrpc": "2.0"` answers `-32600`.

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

**What.** A signed-in person connects an agent from one page, `GET /developer/mcp` (sidebar
Developer → MCP):

- **Create an MCP Key** (`POST /developer/mcp/key`) mints a lake API token through `create_token`,
  the capability `/tokens` uses. *Every tool* scopes it `*`; *Read-only tools* scopes it to every
  read tool's name (`agent_docs::read_tool_names`; `workspace_query` counts as a read). The lifetime
  is 1 day, 7 days, or as long as the sign-in allows (at most 30 days): a token never outlives the
  sign-in that made it (lake#193). The key is shown once and revoked on `/tokens`.
- **Set Up**: copy-ready blocks with the viewer's key and lake's origin for Claude Code (`claude
  mcp add --transport http lake <origin>/mcp --header "Authorization: Bearer <key>"`), Cursor
  (`mcp.json` with `url` and `headers`), Claude Desktop (`mcp-remote` through `npx`), and plain
  JSON-RPC `curl` (`initialize`, `tools/list`). Each block has a **Copy** button: the page's one
  added script, `copy-buttons` (`navigator.clipboard.writeText` of the block's text).
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

**How to verify.** `HttpResponseIsolation.TheMcpPageMakesAKeyThatWorksOverMcp` (llms.txt public;
llms-full.txt documents every `tools/list` tool; a read-only key lists tools, reads, and is refused
a write; the skill downloads) and `WebUi.McpPageShowsTheKeyOnceInEveryBlockAndTheToolsByGroup`.
