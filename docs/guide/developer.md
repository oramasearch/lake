# Developer

Everything you can do on the site is also a REST call and an MCP tool, from one registry.

## Keys

Create a key on **Developer → MCP** (every tool, or read-only). A key acts as you. It lasts as long
as the sign-in that made it, at most 30 days, unless your space allows keys that never expire, in
which case you can choose that lifetime.

## REST

- A read is `GET /v1/<tool>?<inputs>`; an action is `POST /v1/<tool>` with a JSON body.
- Every answer is `{"ok": true, "value": …}` or `{"ok": false, "error": {"code", "message", "path"}}`.
- The full schema: [/v1/openapi.json](/v1/openapi.json).
- `workspace_query` and `workspace_act` are POST: they reach your workspace's own capabilities with
  `{"capability": "…", "input": {…}}`.

## MCP

The server is `https://lake.oramadev.net/mcp`. For Claude Code:

```
claude mcp add --transport http lake https://lake.oramadev.net/mcp --header "Authorization: Bearer $LAKE_KEY"
```

**Developer → MCP** has the setup for Cursor and Claude Desktop, the lake skill for Claude, and
worked flows. [llms.txt](/llms.txt) describes lake in one page for any agent.
