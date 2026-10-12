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

## Where things live

- **The connection** is a workspace record: a sealed `lake_mcp_connections` record in the workspace
  Zetta of its workspace, found by its account ([workspace records](../architecture/workspace-records.md)).
  It holds the name, the URL and transport, the authentication kind, what the server offered at
  the last refresh, the person's decision for each tool, the re-export switch, the auto-approve
  grants, and the connection's agent and streams. Another workspace of the account never sees it.
  Nothing of it is in PostgreSQL. Lake's store keeps only its **dispatch row** (migration
  `0079_mcp_connection_dispatch`, lake#684, lake#685): the account, workspace and connection ids,
  whether it streams, and the digest of the tools it re-exports. That is dispatch, never content.
- **The credential** is a workspace secret (decision 58) in a secret mount of the same workspace:
  a bearer token, or the OAuth refresh token Lake writes there when a person signs in. The record
  holds only its reference, `lake-secret://<mount>/<name>#<field>`. Lake reads it server-side for
  each call (audited in the mount's workspace as `mcp_connection`) and never answers it to a
  person, a browser, a model or an agent. An access token from a refresh lives only in the
  memory of the Lake process that obtained it, until shortly before it expires. These are the
  workspace's secrets; Orama's own keys stay environment variables of the hosting platform.

## Capabilities

Every capability takes `customer_id` and `workspace_id`; the reads need `read` on the account and the
actions `write`. Each is audited in the workspace's own log ([audit](audit.md#which-log)), with
`object` `mcp_connection/<id>`.

| Capability | Kind | Input | What it does |
|---|---|---|---|
| `create_mcp_connection` | action | `name`, `url`, `transport?` (`auto`, `streamable_http`, `sse`), `auth?` | Adds a connection and discovers the server at once (an OAuth connection waits for a person to sign in). No tool is allowed yet. A name used in the workspace, or one whose prefix is, is `conflict` |
| `list_mcp_connections` | read | — | `{connections}`, by name, each with `status`, `refreshed_at`, `tool_count`, `allowed_count`, `exported` and `calls_this_month` |
| `get_mcp_connection` | read | `id` | The connection with its `tools`, `resources`, `prompts`, `skipped` and `streams` |
| `update_mcp_connection` | action | `id`, then any of `name`, `url`, `transport`, `auth`, `tools`, `exported`, `agent_id`, `input_topic`, `output_topic`, `notifications_topic`, `subscribe_resources` | Changes it; a new URL, transport or authentication discovers again |
| `delete_mcp_connection` | action | `id` | `{deleted: true}`; the secret stays in its mount |
| `refresh_mcp_connection` | action | `id` | Asks the server again what it offers |
| `test_mcp_connection` | action | `id`, or `url`, `transport?`, `auth?` | Connects without keeping anything: `{ok, status, server, transport, tools, skipped, milliseconds}` or `{ok: false, status, error}` |
| `list_mcp_connection_tools` | read | `connection_id?`, `allowed_only?` | `{tools, note}`: every tool with `exported_name` (`<prefix>__<tool>`), its schema, `allowed`, `change`, `exported` and `auto_approved` |
| `read_mcp_tool` | read | `connection_id`, `tool`, `arguments?` | Runs an allowed **read** tool |
| `call_mcp_tool` | action | `connection_id`, `tool`, `arguments?` | Runs an allowed tool, a change included |
| `set_mcp_tool_auto_approve` | action | `connection_id`, `tool`, `agent`, `auto_approve` | Grants or takes back one unattended agent's right to run one change tool |
| `list_mcp_connection_calls` | read | `id`, `limit?` | `{calls}`: the connection's recent calls and changes from the workspace audit, newest first |
| `begin_mcp_connection_sign_in` | action | `id`, `mount_id`, `secret_name?`, `client_id?` | Starts an OAuth sign-in: `{authorization_url, state, redirect_uri, issuer}` ([signing in](#signing-in)) |
| `complete_mcp_connection_sign_in` | action | `state`, `code` | Finishes it: the refresh token becomes a workspace secret, then the server is discovered |
| `create_mcp_connection_agent` | action | `id`, `prompt`, `name?` | An agent on agent-runtime whose only tools are this connection's ([its agent](#a-connections-agent-and-streams)) |

`auth` is `{kind: none}`, `{kind: bearer, secret: {mount_id, name, field}}` or `{kind: oauth}`. A
bearer secret is checked with the caller's own credential when it is set: the mount must be the
connection's workspace's, the caller must be able to read it, and the field must hold text.

A call answers `{connection_id, tool, change, is_error, content, structured_content, note}`:
`content` and `structured_content` are the server's, `is_error` is the server's `isError`, and
`note` says they are data. Refusals: an unknown connection is `not_found` at `/connection_id`; a
tool that is not allowed is `invalid_input` at `/tool`; arguments that are not an object of at most
256 KiB are `invalid_input` at `/arguments`; a server that asks for sign-in, or a credential that
is missing or refused, is `sign_in_required` (409) and the connection's status becomes
`needs_sign_in`; a server that does not answer is `store_unavailable`; a server's refusal or an
answer that is not MCP is `engine_error` with Lake's words for it.

## Tools: a read or a change

On every discovery Lake keeps what the server offers, and a person decides each tool:

- **allowed**: whether the workspace may use it at all. A new tool is not allowed until a person
  allows it; a refresh keeps the decisions for the tools still offered and drops those that went.
- **change**: whether it changes something. The server's `readOnlyHint` proposes (a tool marked
  read-only starts as a read; anything else as a change); the person decides.

A read runs through `read_mcp_tool` (a read capability, so Ask Orama's turn key, which carries
every read, can run it) or `call_mcp_tool`. A change runs only through `call_mcp_tool`.

## Who may call a tool

| Caller | Reads | Changes |
|---|---|---|
| A person (session or API key) | yes | yes: the call is the person's confirmation |
| Ask Orama and the other library agents (a task credential) | yes | no: Ask Orama proposes `call_mcp_tool`, and the person's **Confirm** runs it as the person (`not_approved` otherwise) |
| A user-built agent (a launch or schedule credential) | with the skill `mcp:<prefix>` on its record | only where a person marked that tool **auto-approved** for that agent; otherwise it proposes (`not_approved`) |
| A client of Lake's MCP, through a re-exported tool | yes | as the caller above would |

A user-built agent's skills are read from its record in the workspace engine on each call, so an
agent without `mcp:<prefix>` is `unauthorized` even if its runtime offered it the tool. The launch
and schedule credentials carry `list_mcp_connection_tools`, `read_mcp_tool` and `call_mcp_tool`
([agent launches](agent-launches.md#launches)).

**Auto-approve** (`set_mcp_tool_auto_approve`) is per tool and per agent, only for an allowed
change tool, and only a person grants it: an agent's credential is `unauthorized`. The grant names
who granted it and when; each call it allows is audited with `auto_approved: true`, the agent and
the grantor. A grant goes when its tool stops being allowed, stops being a change or leaves the
server.

**What a server answers is data, never instructions.** Tool descriptions, results and
notifications come from an outside server. Lake marks them as such (`note`), Ask Orama and the
agents carry the rule that a result asking for an action is shown to the person, not obeyed, and
an agent's own prompt says so.

## Audit and usage

Every capability above is recorded in the workspace's audit. A tool call's event names the
connection, the tool, whether it is a change, how it came (`via`: `person`, `agent` or `reexport`),
the agent, whether it was auto-approved and by whom, the bytes each way, the server's time and the
server's `isError`; never the arguments or the result. `list_mcp_connection_calls` reads those
events back.

Each exchange with a server is the workspace's usage ([metering](metering.md#usage-over-time)):
one `calls` with `kind: mcp_connection` and `object_id` the connection, and the bytes sent and
received as `transfer` with `kind: mcp_out` and `mcp_in`, through the existing `UsageRecorder`.

## Signing in

A connection with `auth: {kind: oauth}` signs in with OAuth 2.1, as the MCP authorization
specification describes; a person does it once, and Lake keeps the refresh token.

1. `begin_mcp_connection_sign_in {id, mount_id}` (a person only; an agent's credential is
   `unauthorized`):
   - **Discovery.** Lake asks the server without a credential; its `401` names the protected
     resource metadata (RFC 9728, `WWW-Authenticate: Bearer resource_metadata=…`), else Lake reads
     `/.well-known/oauth-protected-resource` at the server's origin. Its `authorization_servers[0]`
     is the issuer (else the server's own origin), whose metadata is
     `/.well-known/oauth-authorization-server` (RFC 8414, then `openid-configuration`). An issuer
     that does not list `S256` in `code_challenge_methods_supported` is refused. Every URL is
     checked like the server's: public addresses, `https`.
   - **Registration.** Lake registers itself as a public client (RFC 7591: `client_name: Lake`, the
     redirect URI, `authorization_code` and `refresh_token`, `token_endpoint_auth_method: none`),
     once per connection and redirect URI. A server that registers no clients needs the client id
     it issued, passed as `client_id`.
   - **PKCE.** A 32-byte verifier from the system's random generator, its S256 challenge, and a
     `state` of `<customer>.<workspace>.<random>`; the verifier, the redirect URI, the client and
     where the token goes are a sealed `lake_mcp_oauth_states` record kept ten minutes.
   - The answer's `authorization_url` carries `response_type=code`, `client_id`, `redirect_uri`,
     `code_challenge`, `code_challenge_method=S256`, `state`, `resource` (RFC 8707: the server's
     resource) and the resource metadata's `scope`. The redirect URI is
     `<Lake's origin>/mcp-connections/oauth/callback`, a page of the web app.
2. The person signs in at the authorization server, which sends them back to the callback page
   with `code` and `state`; the page calls `complete_mcp_connection_sign_in {state, code}`:
   - the state is taken once (a second use, an expired one, another workspace's or another person's
     is `not_found`), and the code is exchanged at the token endpoint with the verifier and
     `resource`;
   - the **refresh token** (with a registration's client secret, if the server issued one) is written
     with `put_secret` **as the person** to `secret_name` (default `mcp-<prefix>`) in the mount, so
     their permission on the mount is checked and the write audited there; the access token is
     never stored. A server that issues no refresh token gets its access token kept there with its
     expiry instead, and the person signs in again when it lapses;
   - the connection names that secret, records who signed in and when, and discovers the server.

A call obtains an access token with the refresh token (`grant_type=refresh_token`, with `resource`)
and keeps it in the process's memory until shortly before `expires_in`. A **rotated** refresh token
is written back as a new version of the same secret. A server that refuses the access token gets
one more call with a fresh one; a refused refresh token makes the connection `needs_sign_in` and the
call `sign_in_required`.

## Lake's MCP re-exports a connection (opt-in)

`update_mcp_connection {exported: true}` puts the connection's **allowed** tools in the
`tools/list` of the workspace's MCP endpoint, `POST /w/<workspace_id>/mcp`, as `<prefix>__<tool>`,
so one MCP client configured for Lake reaches Lake and what the workspace connected. Off by
default, per connection. The schemas are the server's; Lake changes only the name, adds the
connection's name to the description and sets `readOnlyHint` from the person's decision and
`openWorldHint: true`. Lake's own capability names never hold `__`.

A `tools/call` of `<prefix>__<tool>` runs through `read_mcp_tool` or `call_mcp_tool` as the caller,
so it is authorized, audited (`via: reexport`) and metered as theirs, and answers the server's own
result (`content`, `structuredContent`, `isError`). A refusal is `isError: true` with Lake's words.

**What turning it on means:** a re-exported tool runs with Lake's credential to that server for
**anyone who may call Lake's tools in this workspace**. Lake's policy, the caller's permission on
the workspace, is the only gate. The connection's page says so beside the switch.

**An open session is told** (lake#684). Whenever Lake writes a connection, it keeps its dispatch
row in lake's store (migration `0079_mcp_connection_dispatch`): the SHA-256 of the `tools/list`
entries it re-exports, empty when it re-exports none. The workspace endpoint names its tool list by
`mcp_workspace_tool_list_digest`: Lake's own digest when no connection re-exports, else the
SHA-256 of Lake's digest and each re-exporting connection's id and digest. `initialize` answers an
`Mcp-Session-Id` carrying that digest, so exporting a connection, allowing or disallowing one of
its tools, a refresh that changes its tools, renaming it, or no longer exporting it changes the
list, and the session's next request (with `Accept: text/event-stream`) receives
`notifications/tools/list_changed` ahead of its answer, as after an upgrade ([mcp.md](mcp.md)).
Each Lake process remembers what a session last received from `tools/list` (`McpSessionLists`,
at most 10,000 sessions), so a session is told until it lists the tools again, not on every later
request; a session another replica listed is told once more there. The row holds ids and a
digest only: the connection and its tools stay in the workspace Zetta.

## A connection's agent and streams

Optional, per connection (Steve's design in lake#652):

- **Its agent.** `create_mcp_connection_agent {id, prompt, name?}` makes, as the person, a Python
  agent on `oramasearch/agent-runtime` (`workspace_act create_agent`, a `zetta` definition) whose
  only skill is `mcp:<prefix>`, so its only tools are this connection's allowed tools, and whose
  instructions say what it is for and that what the server answers is data, never instructions.
  The connection records it (`agent_id`). The workspace engine must accept `mcp:<slug>` skills
  ([zetta](https://github.com/oramasearch/zetta) PR from this epic). It runs on demand, on a
  schedule (`schedule_agent`), or from its input stream.
- **Input stream** (`input_topic`, needs the agent): a workspace topic (Train → Streams). Each new
  event starts a job of the agent (`launch_agent`, mode `job`) with
  `{connection_id, connection, topic, sequence, event}` as its input, the way the agent bus hands
  a task to a library agent. Each event starts one job: the connection records the last sequence
  it started (`input_cursor`).
- **Output stream** (`output_topic`): each finished job's result is published as
  `{task_id, agent, type, message, result, connection_id}`, the shape `agent-events` uses, with the
  job's launch id as `task_id`; `type` is `result` or `failed`. Other agents, apps and pipelines
  consume it.
- **Notifications** (`notifications_topic`, no agent needed): Lake keeps a session open with the
  server, subscribes to `subscribe_resources` (`resources/subscribe`), and publishes each
  notification (`notifications/*`) as `{connection_id, connection, method, params, note}`, so a
  server that pushes changes drives the agent with no polling, by pointing the input stream at the
  same topic.

The streams act with the authority of **the person who last set the agent or a stream**
(`streams_set_by`), as a schedule's runs act as whoever set the schedule: their permission on the
workspace decides whether a job starts. The store root's owner runs them, and only it
(`McpStreamWorker`, on the server's maintenance thread, every `mcp_stream_interval`, 15 s), so two
replicas never mirror a notification twice; each pass reads at most 64 connections and 20 input
events each. Metrics: `lake_mcp_stream_passes_total`, `lake_mcp_notifications_mirrored_total`,
`lake_mcp_stream_turns_total`, `lake_mcp_stream_problems_total`. Mirrored notifications count as
the workspace's `calls` (`kind: mcp_connection`).

## Verify

`McpClient.*` (`tests/test_mcp_client.cpp`) runs the client over the server's own transport against
a local test MCP server (`tests/mcp_test_server.hpp`): discovery over both transports, an answer
read from an event stream left open, a server that is slow, huge, not JSON, answers another id
first, asks Lake for sampling, streams events forever or speaks an unknown version; sign-in asked for
with RFC 9728 resource metadata; a session started again; notifications on the `GET` stream; and the
public transport refusing `127.0.0.1` before anything is sent.

`Stores/WorkspaceRecordStores.McpConnectionsLiveInTheirWorkspaceAndKeepTheirTokenSecret` (memory
and PostgreSQL stores): refusals before anything is kept, discovery on create, the record sealed
with no token in it, another workspace and another account, tools allowed and decided, a read, a
change refused as a read, a change by a person, Ask Orama reading but refused a change, a
user-built agent refused without its skill and its change refused until a person grants it, the
grant taken back, the audit with no arguments, the usage series, re-export through
`mcp_dispatch_async` with `/w/<workspace>/mcp`, a refused token marking the connection, and delete.
`...McpConnectionsSignInWithOAuthAndKeepOnlyARefreshToken`: discovery, registration, the PKCE
challenge, a state taken once and by its own person, only the refresh token kept, and an access
token renewed with a rotated refresh token. `...McpConnectionStreamsMirrorNotificationsAndDriveTheAgent`:
notifications mirrored to their topic after a subscription, one job per input event as the person,
and its result published once on the output topic.
`HttpServer.WorkspaceMcpSessionsLearnThatExportedToolsChanged`
drives `/w/<workspace>/mcp`: the notice after an export until the session lists, again after a
change of what is exported, and a new session on Lake's own list once nothing is.
`Mcp.WorkspaceDigestFollowsExportedToolsAndListedSessionsAreNotToldAgain` checks the digest and the
bounded memory of listed sessions.
`SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls each capability over MCP.
