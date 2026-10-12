# MCP Setup

Lake is an MCP server. Connect Claude Code, Codex, Cursor or any other client that speaks the Model
Context Protocol, and your agent can use every Lake capability as a tool: look at your datasets,
build and run pipelines, publish a model, read your repositories. Every tool is the same capability
the web app and the REST API call, with the same permissions and the same audit, and the agent acts
as you: it can never do more than the key you give it.

## Concepts

| Term | What it is |
|---|---|
| **Endpoint** | `https://<your lake>/mcp`, for example `https://lake.oramadev.net/mcp`. JSON-RPC 2.0 over streamable HTTP, protocol version `2025-06-18`. |
| **Key** | A Lake API token sent as `Authorization: Bearer <key>`. The same keys work for MCP and REST. |
| **Tool** | One capability. Its name, input schema and output schema are exactly those of the REST operation. |
| **Read tool** | A tool that changes nothing, marked `readOnlyHint: true`. Only a tool that deletes, destroys, removes or revokes something is marked `destructiveHint: true`, so a client asks you before those; a tool that is safe to call twice is marked `idempotentHint: true`. |
| **The Lake skill** | A public `SKILL.md` that teaches an agent what Lake is, which tool does what and the rules to keep. |

Everything below is on one page: **Developer → MCP**. Its strip at the top shows the MCP URL with
**Copy**, the protocol, the header and how many tools there are. The page fills every block with
your Lake's address and your key.

## Create an MCP Key

1. Open **Developer → MCP**. Step 1 is **Create an MCP Key**.
2. Choose what the key may do:
   - **Every tool**: anything you may do yourself.
   - **Read-only tools**: only the read tools; every write is refused. Choose this for an agent that
     should look but not change, and for any key a third party keeps.
   - **Named tools**: only the tools you list, by name.
3. Choose its lifetime: as long as your sign-in allows (at most 30 days), 1 day, 7 days, a number of
   seconds, or **Never**. A key that never expires needs a reason, names its tools, and needs the opt-in
   of the account that owns your workspaces.
4. Press **Create an MCP key**. The key is shown once, under *Your New Key: Copy It Now*, and Lake
   keeps only its hash. Store it in a password manager or your client's secret settings, then
   press **I've stored it**.

Already have a key? Paste it into the field beside the form: it fills the setup blocks and stays in
that page only. A key made on **Developer → API** works here too.

## Set Up Your Client

Step 2, **Set Up Your Client**, has a tile per client. Choose yours; the page shows the file the
setup goes in, the blocks with your address and key filled in, and how to check it worked. Without
a key the blocks read the key from `LAKE_KEY` in your environment, in the syntax each client
expands. The setups below show `https://lake.oramadev.net/mcp`; use your own Lake's address.

### Claude Code

Run in a terminal:

```
claude mcp add --transport http lake https://lake.oramadev.net/mcp --header "Authorization: Bearer $LAKE_KEY"
```

Add `--scope user` to use Lake in every project, or `--scope project` to share it through the
project's `.mcp.json` (without your key in it). Check: run `/mcp` inside Claude Code, or
`claude mcp list` in a terminal; Lake shows *Connected*.

### OpenAI Codex

The CLI and the IDE extension share `~/.codex/config.toml`:

```
[mcp_servers.lake]
url = "https://lake.oramadev.net/mcp"
bearer_token_env_var = "LAKE_KEY"
```

Put `export LAKE_KEY="<your lake key>"` in your shell profile; Codex reads the key from the
environment, so it is never written into the file. Or add it with one command:

```
codex mcp add lake --url https://lake.oramadev.net/mcp --bearer-token-env-var LAKE_KEY
```

Check: `codex mcp list` in a terminal, or `/mcp` inside Codex.

### Claude Desktop

Claude Desktop → Settings → Developer → **Edit Config** opens `claude_desktop_config.json`
(macOS `~/Library/Application Support/Claude/`, Windows `%APPDATA%\Claude\`). Lake connects
through `mcp-remote`, which needs Node.js 18 or later:

```json
{
  "mcpServers": {
    "lake": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://lake.oramadev.net/mcp", "--header", "Authorization:${LAKE_AUTH_HEADER}"],
      "env": {"LAKE_AUTH_HEADER": "Bearer <LAKE_KEY>"}
    }
  }
}
```

Put your key in place of `<LAKE_KEY>`: Claude Desktop does not read your shell's environment. The
header goes through `env` so it survives intact on Windows. Check: restart Claude
Desktop; Lake appears in a new chat's tools menu.

### Cursor

`~/.cursor/mcp.json` for every project, or `.cursor/mcp.json` for one:

```json
{"mcpServers": {"lake": {"url": "https://lake.oramadev.net/mcp", "headers": {"Authorization": "Bearer ${env:LAKE_KEY}"}}}}
```

`${env:LAKE_KEY}` is read from the environment Cursor was started in. Check: Cursor → Settings →
Tools & Integrations (MCP) lists Lake with a green dot.

### VS Code

Copilot's agent mode reads `.vscode/mcp.json` in your project:

```json
{
  "servers": {"lake": {"type": "http", "url": "https://lake.oramadev.net/mcp", "headers": {"Authorization": "Bearer ${input:lake-key}"}}},
  "inputs": [{"type": "promptString", "id": "lake-key", "description": "lake MCP key", "password": true}]
}
```

VS Code asks for the key the first time Lake starts and keeps it in its secret storage; it never
goes into the file, so the file is safe to commit. Check: run *MCP: List Servers* from the Command
Palette.

### Windsurf

`~/.codeium/windsurf/mcp_config.json` (Devin Desktop: `~/.config/devin/mcp_config.json`). Remote
servers take `serverUrl`, not `url`:

```json
{"mcpServers": {"lake": {"serverUrl": "https://lake.oramadev.net/mcp", "headers": {"Authorization": "Bearer ${env:LAKE_KEY}"}}}}
```

Cascade → MCP servers → Refresh picks the file up.

### Gemini CLI

`~/.gemini/settings.json` for every project, or `.gemini/settings.json` for one. `httpUrl` is
streamable HTTP (`url` would be SSE); merge `mcpServers` into the settings you have:

```json
{"mcpServers": {"lake": {"httpUrl": "https://lake.oramadev.net/mcp", "headers": {"Authorization": "Bearer ${LAKE_KEY}"}}}}
```

Or add it with one command:

```
gemini mcp add --transport http --header "Authorization: Bearer $LAKE_KEY" lake https://lake.oramadev.net/mcp
```

Check: `/mcp` inside Gemini CLI, or `gemini mcp list`.

### Any Client

Any client that speaks MCP's streamable HTTP transport and can send an `Authorization` header
connects the same way. With `curl`:

```
curl -s https://lake.oramadev.net/mcp \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'

curl -s https://lake.oramadev.net/mcp \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

curl -s https://lake.oramadev.net/mcp \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_workspaces","arguments":{"customer_id":"<customer id>"}}}'
```

`initialize` answers Lake's `serverInfo`; `tools/list` answers every tool and needs no key;
`tools/call` on anything but `server_info` does.

### ChatGPT and Claude.ai

- **Claude.ai**: Customize → Connectors → **Add custom connector**, with the name `lake`, the remote
  MCP URL, and the request header `Authorization: Bearer <your key>`. Claude's servers call Lake, so
  your Lake must be reachable from the internet, and Anthropic keeps the key: give it a read-only
  key.
- **ChatGPT**: its custom connectors sign in with OAuth or no authentication and cannot carry a Lake
  key, and Lake has no OAuth sign-in for connectors, so ChatGPT cannot connect to Lake. Use OpenAI
  Codex instead.

### After Lake Upgrades

When Lake gains or changes tools, a connected client is told (`notifications/tools/list_changed`)
on its next call, if it accepts server-sent events. If your client keeps showing an old tool list,
reconnect or restart it.

## Teach Your Agent

Step 3 gives your agent Lake's rules, so it calls the right tool in the right order.
**Download the Lake skill** saves the public `/skills/lake/SKILL.md`. For Claude Code, install it
from a terminal:

```
mkdir -p ~/.claude/skills/lake && curl -fsSL https://lake.oramadev.net/skills/lake/SKILL.md -o ~/.claude/skills/lake/SKILL.md
```

Use `.claude/skills/lake/` instead to install it for one project. For Codex and Gemini CLI the page
gives a block that downloads the skill and adds a short section to `~/.codex/AGENTS.md` or
`~/.gemini/GEMINI.md` pointing at it.

For any other agent, `/llms.txt` describes Lake in one page, and `/llms-full.txt` adds every tool's
description and input schema. Both are public, so an agent without a key can read them.

The skill, both text files and this page are generated from the capability registry, so they list
exactly the tools the server has.

## What the Tools Do

Step 4, **What the Tools Do**, lists every tool by product group, with a read or write chip, a
search and a filter. The groups are: Workspace: Design, Train and Ship; Projects and pipelines;
Training and runs; Experiments; Models; Search indexes; Datasets; Connections; Decisions;
Solutions; Agents; Streams; Drives; Transcripts; Search; Apps and packages; Services; Storage;
Repositories; Organizations and access; Usage and billing; Identity and sessions; Secrets, keys and
certificates; Audit and transparency; Engine; Server. Each group says in one line what it holds.

A few things to know:

- Every tool's input schema is the REST operation's, byte for byte. Design, Train, Ship and
  Transcripts tools require `customer_id` and `workspace_id`.
- Everything your workspace's engine does is its own tool: `list_projects`, `check_pipeline`,
  `start_run`, `get_run_report`, `list_models`, `score`, `create_index`, `analyze_dataset`,
  `create_connection`, `create_agent`, `decide` and the rest, each with its own input schema.
  `workspace_query` (any allowed engine read, by the engine's name) and `workspace_act` (any
  allowed engine action) still work. [REST API and OpenAPI](/docs/developer) explains both.
- The repository sync operations that stream bytes (liquid push and pull) are not MCP tools; use
  [the liquid CLI](/docs/cli).
- A refused call is a tool result with `isError: true` carrying the same error envelope REST
  answers, with its `code` and the `path` of the field at fault.

## Worked Flows

Step 5, **Worked Flows**, shows the tools an agent calls, in order, for the jobs people ask most:

| Flow | Tools in order |
|---|---|
| Load a dataset | `create_workspace` once, `workspace_status` until ready, `upload_dataset`, `get_dataset`, `workspace_sql` |
| Train a model and call it | `create_project`, `compile_pipeline`, `approve_pipeline`, `run_pipeline`, `get_run_report`, `expose_model`, `predict` |
| Make a model on its own | `list_methodologies`, `train_model`, `retrain_model`, `register_model` (or `load_drive_model`), `move_model_to_project`, `export_model`, `delete_model` |
| Build and run a pipeline | `create_pipeline`, `edit_pipeline`, `get_pipeline`, `approve_pipeline`, `run_pipeline` |
| Stand up a Postgres service and connect it | `list_service_kinds`, `create_service`, `list_services`, `service_connection`, `connect_service` |
| Create a volume | `list_store_providers`, `create_store`, `list_stores`, `store_connection`, `create_service`, `list_stores` |
| Search a published index | `list_indexes`, `expose_index`, `search_index` |

The first step of loading a dataset as raw `tools/call` requests:

```
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"workspace_status","arguments":{"customer_id":"<customer id>","workspace_id":"<workspace id>"}}}

{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"upload_dataset","arguments":{"customer_id":"<customer id>","workspace_id":"<workspace id>","table_name":"accounts","format":"csv","content":"id,plan\n1,pro\n"}}}
```

Rules the skill gives an agent, which hold for you too: an edited pipeline must be approved again
before it runs; a failed check runs nothing; search needs a published index; deleting destroys data.

## Your Keys

Step 6, **Your Keys**, lists your keys (MCP and API alike) with their access, expiry, last use and
state. **Revoke** stops a key at once, and anything using it. The operator's key is the instance's
admin token, so the page offers no form to the operator.

The same key can be made without the page. `POST /v1/create_token` with a name and the tools it may
call (`expires_in_seconds` is optional and never longer than the sign-in that makes it):

```json
{"name": "MCP key (read-only)", "scopes": ["list_workspaces", "workspace_status", "workspace_query", "workspace_sql"], "expires_in_seconds": 604800}
```

The answer carries the key once. `GET /v1/list_sessions` lists your keys, and
`POST /v1/revoke_session` with a key's `id` revokes it:

```json
{"id": "<key id>"}
```

The MCP tools have the same names and take the same JSON. [Tokens and Keys](/docs/tokens) has the
full rules.

## Limits

- One request body is at most 64 MiB of JSON.
- Calls that wait for events count against 64 long requests per caller; past that a call answers
  `rate_limited`, retry.
- `GET /mcp` and `DELETE /mcp` answer `405`: Lake keeps no session and opens no standalone event
  stream.
- A key never outlives the sign-in that made it, unless it is a standing key.

## Permissions

A tool runs as the person whose key it is, through the same permission checks as the web app: a
tool you may not use on an object answers `unauthorized`, and an object you cannot see answers
`not_found`. A read-only key is refused every write tool. Every call is recorded in the audit log
under your name, and you can read your own on **Organization → Activity**.

## Troubleshooting

- **`unauthenticated` with `reason` `missing`**: the client sends no `Authorization` header. Check
  the header name and that `LAKE_KEY` is set in the environment the client started in.
- **`reason` `expired`**: the key's lifetime passed, or the sign-in that made it ended. Make a new
  key.
- **`reason` `invalid`**: the key is mistyped or revoked.
- **`workspace_id is required`**: the tool works in a workspace. The agent can find one with
  `list_workspaces`. If the client cached an old tool list without the field, reconnect it.
- **The client lists fewer tools than the page counts**: it holds a list from before an upgrade;
  reconnect or restart it.
- **Claude Desktop shows Lake failing to start**: `mcp-remote` needs Node.js 18 or later, and the
  key must be written into the file.
- More answers: [FAQ and Troubleshooting](/docs/faq).

## Related Pages

- [REST API and OpenAPI](/docs/developer)
- [Tokens and Keys](/docs/tokens)
- [Ask Orama](/docs/ask-orama)
- [The liquid CLI](/docs/cli)
- [Security Model](/docs/security)
- [FAQ and Troubleshooting](/docs/faq)
