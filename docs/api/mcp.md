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

## Developer → MCP, llms.txt and the Lake skill (lake#202)

**What.** A signed-in person connects an agent from one page of the web app, `/developer/mcp`
(sidebar Developer → MCP), which reads `mcp_setup`:

- **Create an MCP Key** mints a lake API token through `create_token`, the capability `/tokens`
  uses, with the scopes `mcp_setup` lists. *Every tool* scopes it `*`; *Read-only tools* scopes it to every
  read tool's name (`agent_docs::read_tool_names`; `workspace_query` counts as a read). The lifetime
  is 1 day, 7 days, or as long as the sign-in allows (at most 30 days): a token never outlives the
  sign-in that made it (lake#193). The key is shown once and revoked on either Developer page's
  Keys tab or on `/tokens` (lake#561, below).
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
  or **Never** (a standing key with a reason and named tools, where the account allows it, lake#234,
  lake#259).
- **Worked Flows**: load a dataset (`upload_dataset` through `workspace_act`), train a model and call
  it (`create_project` → `compile_pipeline` → `approve_plan` → `run_pipeline` → `get_run_report` →
  `expose_model` → `predict`, the Train Docs' recipe, lake#591), keep files in a drive
  (`create_drive` → `upload_drive_entries` → `list_drive_entries` → `read_drive_file`), keep a secret
  (`create_secret_mount` → `put_secret` → `list_secrets` → `read_secret`), keep a transcript
  (`create_transcript` → `list_transcripts` → `get_transcript`) (the last three lake#594), build and run a pipeline (`create_pipeline` → `edit_pipeline` →
  `get_pipeline` → `workspace_act` `approve_plan` → `run_pipeline`), stand up and connect a
  Postgres service (`list_service_kinds` → `create_service` → `list_services` →
  `service_connection` → `connect_service`), a drive of each kind (`create_drive` with
  `storage: repository`, then `virtual` → `create_drive_shortcut` → `list_drives` →
  `load_drive_dataset`), search everything (`search` → `search_drives` → `list_transcripts` `q` →
  `list_docs` `q`), see usage and the bill (`usage_over_time` → `usage_limits` → `price_list` →
  `bill_preview` → `list_invoices`) (these three lake#642), search a published index
  (`workspace_query` `list_indexes` → `workspace_act` `expose_index` → `search_index`).
- **What the Tools Do**: every MCP tool in product groups (Workspace: Design, Train and Ship,
  Experiments, Solutions, Agents, Streams, Drives, Transcripts, Search, Apps and packages, Services,
  Storage, Repositories, Organizations and access, Usage and billing, Identity and sessions,
  Secrets, keys and certificates, Audit and transparency, Server), each with its one-line summary,
  and each tool with its title and read or write.

**Groups are deliberate (lake#642).** Each group in `agent_docs.cpp` names the capabilities it
holds; none takes "the rest". A capability no group claims is listed under `Other`, and
`Docs.EveryCapabilityHasADeliberateGroup` fails until a group claims it, so a new capability is
placed on purpose. Drives (every `*drive*` capability, `load_drive_dataset` among them) and Usage
and billing (`usage_*`, `price_list`, `set_price_list`, `bill_preview`, the invoices, metering and
the storage quota) have their own groups; issues stay with Repositories.

`GET /llms.txt` and `GET /llms-full.txt` are **public** (`text/plain`, cached 5 minutes):
what Lake is and what it does today (Data, Design, Train, Ship, Ask Orama, organizations and
access, secrets, usage and billing), how to connect, every tool by group (llms-full adds each tool's description and input
schema), the flows and the rules an agent must keep (an edited pipeline is proposed again, a gate
failure runs nothing, search needs a published index, deleting destroys data). Their own words
say **Lake** and never "space" (the account, or the account or organization, instead);
`Docs.AgentDocsSayLakeAndNeverSpace` checks the summary, Connect, the group summaries, the flows
and the rules, that llms-full.txt carries every tool with its input schema, and that the skill
names every group. The MCP server name (`claude mcp add … lake`) and the skill's `name: lake` stay
lowercase: they are identifiers.

**One source.** The page, both text files and the skill are generated by
`lake::api::agent_docs` (`include/lake/api/agent_docs.hpp`) from the capability registry, so they
list exactly the tools `tools/list` serves (the JSON capabilities; the liquid sync and ACME
protocol capabilities are not MCP tools). A new capability appears everywhere with no edit; a new
group or flow is an edit to `agent_docs.cpp`.

**How to verify.** `HttpResponseIsolation.AnMcpKeyWorksOverMcp` (llms.txt public; llms-full.txt
documents every `tools/list` tool; a key made with `create_token` and `mcp_setup`'s read-only
scopes lists tools, reads, and is refused a write; the skill downloads).

## Developer → API and Developer → MCP (lake#561)

**What.** The web app's Developer section is two full pages, built the way oramasearch/rev-os builds
its own (`frontend/src/pages/developer/`). Before, the rail's **API** item opened the raw
`/v1/openapi.json` and MCP was one long page.

- **Developer → API** (`/developer/api`, `ApiPage.tsx`). The header downloads `openapi.json`
  (pretty-printed), opens the API guide (`/docs/developer`) and Developer → MCP. Three tabs, kept in
  `?tab=` so a link opens one:
  - **Keys**: the key form, the new key once, and the viewer's keys (below).
  - **Quick start**: copy-ready `curl` and `fetch` against `viewer.external_origin` (else the page's
    origin) for `GET /v1/viewer`, a list read (`list_repositories`), an action
    (`create_repository`, with the viewer's space and workspace filled in) and a workspace-scoped
    call under `/w/<workspace id>/v1`, with the envelope and the error codes explained.
  - **Operations**: every operation of `/v1/openapi.json` as the browser's session receives it,
    grouped by the product group `mcp_setup` gives its tool (liquid sync and the operations that
    are not MCP tools get groups of their own), searchable by path, name, summary or group. Each
    opens to its description, read or write (an action with `x-lake-action: read`, such as
    `workspace_query`, is a read), the permission it needs, its parameters, its request body schema,
    the `value` schema inside the success envelope, and a **Copy as curl** block with the required
    fields as placeholders and the new key, if one was made on this page.
- **Developer → API** also opens with a strip: the base URL (`<origin>/v1`) with Copy, and chips for
  REST · JSON, OpenAPI 3.1, the `{ok, value, duration_ms}` envelope and `Authorization: Bearer <key>`.
  Operations filters by read or write with counts, beside the search.
- **Developer → MCP** (`/developer/mcp`, `McpPage.tsx`) is one page, as rev-os's is: an endpoint
  strip (the MCP URL with Copy; JSON-RPC 2.0, streamable HTTP, the protocol version, `Authorization:
  Bearer <key>` and the tool count), then six numbered steps, each with its title and a line of
  advice in a left rail and the content on the right (one column on a phone):
  1. **Create an MCP Key**: the key form; the new key once, in a highlighted callout (`#mcp-key`);
     or paste a key you have. The operator sees that its key is the admin token, and can paste it.
  2. **Set Up Your Client**: a tile per client (a lucide icon, never a vendor logo; the name; how it
     connects). The chosen tile shows the file the setup goes in (with Copy path), copyable blocks
     with lake's origin and the key filled in, one line on how to check it worked, and what to know.
     With no key the blocks read `LAKE_KEY` in the syntax each client expands. The clients, checked
     against each vendor's documentation on 2026-10-10:
     - *Claude Code*: `claude mcp add --transport http lake <origin>/mcp --header "Authorization:
       Bearer <key>"` (`#mcp-claude-code`), `--scope user` or `project`; check with `/mcp` or
       `claude mcp list`.
     - *OpenAI Codex* (CLI and IDE): `~/.codex/config.toml` `[mcp_servers.lake]` with `url` and
       `bearer_token_env_var = "LAKE_KEY"`, `export LAKE_KEY=…`, or `codex mcp add lake --url …
       --bearer-token-env-var LAKE_KEY`; check with `codex mcp list` or `/mcp`.
     - *Claude Desktop*: `claude_desktop_config.json` (macOS `~/Library/Application
       Support/Claude/`, Windows `%APPDATA%\Claude\`) running `npx -y mcp-remote <origin>/mcp
       --header Authorization:${LAKE_AUTH_HEADER}` with `env.LAKE_AUTH_HEADER = "Bearer <key>"`, the
       form mcp-remote documents so the space survives on Windows.
     - *Cursor*: `~/.cursor/mcp.json` `mcpServers.lake` with `url` and `headers` (`${env:LAKE_KEY}`
       without a key).
     - *VS Code* (Copilot agent mode): `.vscode/mcp.json` `servers.lake` `type: "http"`, the header
       `Bearer ${input:lake-key}` and a `promptString` input with `password: true`, so the key is
       never in the file; check with *MCP: List Servers*.
     - *Windsurf*: `~/.codeium/windsurf/mcp_config.json` (Devin Desktop:
       `~/.config/devin/mcp_config.json`) with `serverUrl` and `headers`.
     - *Gemini CLI*: `~/.gemini/settings.json` `mcpServers.lake` with `httpUrl` (streamable HTTP)
       and `headers` (`${LAKE_KEY}` expands), or `gemini mcp add --transport http --header …`; check
       with `/mcp` or `gemini mcp list`.
     - *Any client*: JSON-RPC `curl` for `initialize`, `tools/list` and `tools/call`.
     - *ChatGPT and Claude.ai*: Claude.ai's custom connectors can send a fixed request header, so
       lake connects there with `Authorization: Bearer <key>` when lake is reachable from the
       internet (a read-only key is advised: Anthropic keeps it). ChatGPT's custom connectors sign
       in with OAuth or no authentication and cannot carry a lake key, so the page sends ChatGPT
       users to Codex. Lake has no OAuth flow for connectors.
     Then *After lake upgrades*: reconnect or restart the client (lake#441).
  3. **Teach Your Agent**: the downloads (the skill, `llms-full.txt`, `llms.txt`, OpenAPI
     operations) and an install per agent, chosen to match the client in step 2: Claude Code's
     skills folder (`~/.claude/skills/lake/SKILL.md`), an `AGENTS.md` section for Codex and a
     `GEMINI.md` section for Gemini CLI that point at the downloaded skill, or the three public URLs.
  4. **What the Tools Do** (`#tools`): search, read/write chips with counts, a chip per tool group
     with its count; each group with its summary and read and write counts, each tool a row with its
     read or write chip, its name in mono and its title; a group longer than eight shows six and
     *Show all*, unless a filter is on.
  5. **Worked Flows**: the flows from `mcp_setup`.
  6. **Your Keys** (`#keys`): the keys table, as on Developer → API.
  `?tab=keys` and `?tab=tools`, the former tabs' addresses, scroll to their step. The setups are
  pure builders in `logic.ts` (`CLIENTS`, `clientSetup`, `skillInstall`).
- **Keys** (`parts.tsx`, rules in `logic.ts`). One form for both pages calls `create_token` with
  what `/tokens` and the former MCP form sent: a name (default *API key* or *MCP key*, with
  *(read-only)*); access *every tool* (`*`), *read-only* (`mcp_setup.scopes.read_only`) or *named
  capabilities* (comma separated, with the liquid push and pull set one click away); a lifetime of
  the sign-in's (at most 30 days), 1 day, 7 days, a number of seconds or **Never**; and *static*. A
  standing or static key needs a reason, and a standing *every tool* key names every capability
  (`scopes.all`) because a key that never expires cannot be `*`. The secret is shown once in a
  callout and fills the page's code blocks until the page is left. The table lists the viewer's
  `api_token` and `static` sessions from `list_sessions` (name, access, expiry, last used, created,
  state) and revokes one through `revoke_session` after a confirmation.
- **The operator** has no tokens: its key is the instance's admin token (`LAKE_ADMIN_TOKEN`). Both
  Keys tabs say so instead of offering the form.

**Why.** A developer needs the keys, the contract and a first call in one place; a raw JSON
document in a new tab gave them only the contract.

**How to verify.** From `frontend/`: `npx tsc --noEmit -p .`, `npm test` (`tests/developer.test.ts`
checks the `create_token` inputs, how scopes read, the grouping, the envelope's `value` schema,
the curl lines against the shape `openapi_document` renders, and that every client setup names the
origin and the key or `LAKE_KEY`, that its JSON parses with the fields that client reads, and that
every skill install fetches the public `SKILL.md`) and `npm run build`. In a browser:
make a read-only key in step 1 of Developer → MCP, copy the Claude Code block, call
`tools/list` with it, and revoke it from either Keys tab.

**How to change.** A new capability appears in Operations and Tools with no edit. A new MCP client
is an entry in `CLIENTS` and a case in `clientSetup`, an icon in `McpPage.tsx`'s `ICONS`, and its
syntax checked against the client's own documentation. The page
structure is in `ApiPage.tsx` and `McpPage.tsx`; the key rules and the OpenAPI reading in
`logic.ts`, which has no imports so the tests run it directly.
