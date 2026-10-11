# MCP Connections

**Status: implemented 2026-10-11 in the source tree (decision 60, lake#652); not deployed.** Lake is
an MCP **server** (`POST /mcp`, every capability a tool, [mcp.md](mcp.md)). It is also an MCP
**client**: a workspace connects to remote MCP servers, and its people, Ask Orama and its agents use
their tools through Lake. Lake runs the client server-side, so the browser and agents never hold the
server's credential, and every call is authorized by Lake's policy, audited in the workspace and
metered.

## The client

Lake speaks MCP itself (`include/lake/api/mcp_client.hpp`, `src/api/mcp_client.cpp`); no SDK. The
HTTP exchange is an injected `McpHttp`: the server's own transport in `src/api/http_server.cpp` (the
one translation unit that names HTTP types), or a test's.

**Transports.** A connection reaches a **remote** server:

- **Streamable HTTP** (protocol `2025-06-18`, also `2025-03-26`): every request is a `POST` to the
  server's URL with `Accept: application/json, text/event-stream`; the answer is one JSON object or
  an event stream that carries it. After `initialize` the client sends the server's
  `Mcp-Session-Id` and `MCP-Protocol-Version` on every request, and ends the session with `DELETE`.
  A `404` on a session starts a new session once.
- **HTTP+SSE** (protocol `2024-11-05`), for older servers: a `GET` opens the session's event
  stream, whose `endpoint` event names where requests are posted; answers come on the stream. The
  endpoint must have the server's own origin.
- `transport: auto` (the default) tries Streamable HTTP and falls back to HTTP+SSE when the server
  refuses the `POST` of `initialize` with `400`, `404` or `405`.

A server that only speaks stdio is not supported yet: it will run as a workspace App container that
Lake reaches over HTTP ([lake#659](https://github.com/oramasearch/lake/issues/659)).

**What Lake asks.** `initialize` (Lake declares no client capabilities: no sampling, roots or
elicitation), `notifications/initialized`, then `tools/list`, `resources/list` and `prompts/list`
where the server declares them, following `nextCursor`; `tools/call`, `resources/read`,
`prompts/get`, `resources/subscribe`; and the server's notifications, which arrive on an answer's
event stream or on the session's `GET` stream.

**The server is not trusted.** Each bound is in `McpLimits`:

| Bound | Default | What happens past it |
|---|---|---|
| One exchange, end to end (DNS, connect, TLS, answer) | 30 s | `timeout` |
| One answer or event stream | 4 MiB | `too_large`; reading stops |
| Events read while waiting for one answer | 512 | the call fails; a server that streams progress forever cannot hold it |
| Pages of each list | 10 | the rest is not read |
| Tools kept per connection | 128, and 512 KiB in all | the rest are skipped, with a reason |
| A tool's title / description / input schema | 256 B / 2 KiB / 16 KiB | cut at a character / cut / the tool is skipped |

An answer counts only when it answers the request's own `id`: an answer to another id is ignored. A
request from the server (`sampling/createMessage`, `roots/list`, `elicitation/create`) is never
served, since Lake declared none of them. A tool whose name is not an MCP tool name
(`[A-Za-z0-9_.-]{1,128}`), whose input schema is not an object schema, or that is listed twice is
skipped, and `skipped` says why. A server that keeps an event stream open after answering does not
hold the call: the transport stops reading as soon as the answer is in.

**Addresses.** The server URL is `https://` (no user name, no fragment, at most 2048 bytes). Every
address its name resolves to must be public: loopback, private (RFC 1918, unique local), link-local
(the cloud metadata address `169.254.169.254` included), carrier-grade NAT, multicast and reserved
addresses are refused, also through IPv4-mapped and NAT64 IPv6, so a connection cannot reach Lake's
own network (its Postgres, the workspace Zettas). The check runs on the addresses actually
connected to, after resolution, so a name that resolves to a private address is refused however it
was spelled. A Lake started with `LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1` (local development and tests
only, never a deployment) also reaches loopback and private addresses, and plain `http://` to
loopback.

## Verify

`McpClient.*` (`tests/test_mcp_client.cpp`) runs the client over the server's own transport against
a local test MCP server (`tests/mcp_test_server.hpp`): discovery over both transports, an answer
read from an event stream left open, a server that is slow, huge, not JSON, answers another id
first, asks Lake for sampling, streams events forever or speaks an unknown version; sign-in asked for
with RFC 9728 resource metadata; a session started again; notifications on the `GET` stream; and the
public transport refusing `127.0.0.1` before anything is sent.
