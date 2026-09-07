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
and returns the conflict digest, selected value digest, and change digest. Both schemas come from
the registry and therefore appear unchanged in `tools/list`.

```sh
curl -H 'content-type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' https://lake.oramadev.net/mcp
curl -H "Authorization: Bearer <token>" -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_customers","arguments":{"limit":5}}}' \
  https://lake.oramadev.net/mcp
```
