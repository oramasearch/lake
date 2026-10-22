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

A server that only speaks **stdio** runs as an App of the workspace, which Lake reaches over
Streamable HTTP inside the cluster ([below](#servers-that-speak-only-stdio), lake#659).

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
| `create_mcp_connection` | action | `name`, then `url`, `transport?` (`auto`, `streamable_http`, `sse`), `auth?`; or `stdio` ([stdio servers](#servers-that-speak-only-stdio)) | Adds a connection and discovers the server at once (an OAuth connection waits for a person to sign in; a stdio server is `starting` until its App is ready). No tool is allowed yet. A name used in the workspace, or one whose prefix is, is `conflict` |
| `list_mcp_connections` | read | — | `{connections}`, by name, each with `status`, `refreshed_at`, `tool_count`, `allowed_count`, `exported` and `calls_this_month` |
| `get_mcp_connection` | read | `id` | The connection with its `tools`, `resources`, `prompts`, `skipped` and `streams` |
| `update_mcp_connection` | action | `id`, then any of `name`, `url`, `transport`, `auth`, `tools`, `exported`, `agent_id`, `input_topic`, `output_topic`, `notifications_topic`, `subscribe_resources` | Changes it; a new URL, transport or authentication discovers again. A new URL keeps no credential ([moving a connection](#moving-a-connection)) |
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
   - **Whose token** (lake#927). The resource metadata's `resource` must name this server: its URL
     without the query (one trailing slash aside) or its origin, with scheme, host and port compared
     as Lake normalizes them and the path byte for byte. Metadata with no `resource`, or one naming
     another API, host, port or path, stops the sign-in (RFC 9728 section 3.3); otherwise a malicious
     server could have Lake obtain another API's token and send it that token. The authorization
     server's metadata must name the same issuer Lake asked (RFC 8414 section 3.3), or the sign-in
     stops too. Nothing is registered and no one is redirected when either check fails.
   - **Registration.** Lake registers itself as a public client (RFC 7591: `client_name: Lake`, the
     redirect URI, `authorization_code` and `refresh_token`, `token_endpoint_auth_method: none`),
     once per connection and redirect URI. A server that registers no clients needs the client id
     it issued, passed as `client_id`.
   - **PKCE.** A 32-byte verifier from the system's random generator, its S256 challenge, and a
     `state` of `<customer>.<workspace>.<random>`; the verifier, the redirect URI, the client,
     where the token goes, and the server URL, issuer, token endpoint and resource the sign-in was
     begun for are a sealed `lake_mcp_oauth_states` record kept ten minutes.
   - The answer's `authorization_url` carries `response_type=code`, `client_id`, `redirect_uri`,
     `code_challenge`, `code_challenge_method=S256`, `state`, `resource` (RFC 8707: the server's
     resource) and the resource metadata's `scope`. The redirect URI is
     `<Lake's origin>/mcp-connections/oauth/callback`, a page of the web app.
2. The person signs in at the authorization server, which sends them back to the callback page
   with `code` and `state`; the page calls `complete_mcp_connection_sign_in {state, code}`:
   - the state is taken once (a second use, an expired one, another workspace's or another person's
     is `not_found`), and the code is exchanged at the token endpoint the state names, with the
     verifier and the state's `resource`. A connection whose URL, issuer, token endpoint or
     resource changed after the sign-in began is `conflict`, and the code is never sent;
   - the **refresh token**, beside a registration's client secret (if the server issued one) and
     the issuer, token endpoint and resource it was issued for, is written with `put_secret`
     **as the person** to `secret_name` (default `mcp-<prefix>`) in the mount, so
     their permission on the mount is checked and the write audited there; the access token is
     never stored. A server that issues no refresh token gets its access token kept there with its
     expiry instead, and the person signs in again when it lapses;
   - the connection names that secret, records who signed in and when, and discovers the server.

A call obtains an access token with the refresh token (`grant_type=refresh_token`, with `resource`)
and keeps it in the process's memory until shortly before `expires_in`. Lake sends a token only
when the connection's resource still names the connection's URL and the secret, and the cached
access token, name the issuer, token endpoint and resource the connection has now (lake#927). A
connection moved to another address, or re-discovered under another issuer, is
`sign_in_required` until a person signs in again. A secret written before lake#927 names none of
them, so each OAuth connection signed in before then needs one more sign-in. A **rotated** refresh token
is written back as a new version of the same secret. A server that refuses the access token gets
one more call with a fresh one; a refused refresh token makes the connection `needs_sign_in` and the
call `sign_in_required`.

### Moving a connection

A credential belongs to the address it was given for (lake#928). `update_mcp_connection` with a
`url` other than the connection's:

- for a **bearer** connection, needs the same update to name its secret in `auth`, which is checked
  as on create (the caller must be able to read it, in a mount of the connection's workspace), and
  is otherwise `invalid_input` at `/auth` with nothing changed;
- for an **OAuth** connection, drops in the same conditional write its secret, issuer, endpoints,
  client, sign-in and cached access token, and marks it `needs_sign_in`. A sign-in begun before
  the move is `conflict` at completion, since the connection no longer names its server, issuer
  and token endpoint, and moving back restores nothing.

So a person who may change a connection but not read its secret cannot send the bearer, the
refresh token, a registration's client secret or a pending sign-in's code and verifier to a server
they run. The same URL again, a new name, tools or streams keep the credential. The audit records
the new `url`.

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

The stream worker requires the person who set the streams to remain present and enabled
(lake#951, LSR-086). Disabling them immediately blocks input-triggered launches, notifications and
result publication. On the next reachable pass it clears their streaming settings, subscriptions
and pending results and updates the dispatch row, while retaining the connection. The existing
records preserve pending cleanup across restarts and storage outages; another person may set up
streams again under their own authority. See [offboarding](clerk.md#webhooks).

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
  it started (`input_cursor`). The job's launch says `trigger: stream`. Any Python agent can be
  started by a workspace stream the same way, without a connection (`set_agent_streams`,
  [agent-launches](agent-launches.md#stream-triggers)); both workers start their jobs through one
  function (`start_stream_runs`).
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
events each.

**Only the workspaces that stream are called** (lake#685). The connection's dispatch row says it
`streams` when it has a notifications topic, an agent with an input topic, or finished jobs'
results still to publish; Lake writes it with every connection it writes, on any replica. Each pass
reads the streaming rows from lake's store and lists the connections of those workspaces only, one
workspace Zetta call each: a workspace with no streaming connection is never called. A row whose
connection is gone, or no longer streams, is put right by the pass that finds it, and a deleted
workspace's rows go with it. A connection whose streams were set before this change has no row
until it is next saved. Metrics: `lake_mcp_stream_passes_total`, `lake_mcp_notifications_mirrored_total`,
`lake_mcp_stream_turns_total`, `lake_mcp_stream_problems_total`. Mirrored notifications count as
the workspace's `calls` (`kind: mcp_connection`).

## Servers that speak only stdio

Decision 60 took remote servers first; lake#659 adds the servers that only speak stdio, an npm or
Python package such as most reference servers. Lake runs one as an **App of the workspace**
([Ship Apps](web-apps.md)) whose image is Lake's own runner, `oramasearch/lake-mcp-runner` (a
private repository and a private image of its own; we write our own, standard library Go, no MCP
SDK). The runner starts the package as its child, initializes it once, and serves it over
Streamable HTTP at `/mcp`; Lake's client reaches it as any other server.

`create_mcp_connection {name, stdio}`, with `stdio`:

| Field | What |
|---|---|
| `runtime` | `node`: an npm package, run with `npx --yes`; `python`: a Python package, run with `uvx` |
| `package` | the package, with its version (`@modelcontextprotocol/server-everything@2026.8.31`, `mcp-server-fetch==2025.4.7`): letters, digits and `@/._~=<>!+-`, at most 214 bytes, not starting with `-` |
| `command?` | the package's command when it is not named after the package (`npx --package <package> <command>`, `uvx --from <package> <command>`) |
| `args?` | at most 32 strings, after the command |
| `env?` | at most 50 variables, each a string or `{secret: {mount_id, name, field}}`; a name is letters, digits and `_`, and `MCP_`, `APP_` and `LAKE_` are Lake's |
| `mount_id` | the secret mount Lake keeps the runner's secret in |

What Lake does, as the person who adds it (so their permission on the mount, the workspace and its
Apps decides, and each step is audited as theirs):

1. Writes a new secret `mcp-<prefix>-runner` in the mount (`put_secret`): `token`, 32 random bytes
   the runner requires as a bearer on every request, and `command`, the command line as a JSON
   array (a secret field, because a package name with its version reads as a high-entropy value to
   the App's scan of plain variables).
2. Creates the App (`create_app`): `mode: image`, the image `LAKE_MCP_RUNNER_IMAGE`, `framework:
   mcp`, port `8080` (the port the workspace's network policy admits Lake to, as for an agent), the
   name `mcp-<prefix>-<4 hex>`, and the environment: `MCP_RUNNER_TOKEN` and `MCP_STDIO_COMMAND` from
   that secret, and the person's variables, plain or from secrets, which the operator projects from
   the workspace's secret store as for any App (decision 58, lake#625).
3. Keeps the connection: its URL is the App's Service in the cluster,
   `http://app-<the first 12 hex digits of the App's id>.<the workspace's namespace>.svc.cluster.local/mcp`,
   its transport `streamable_http`, its authentication the runner's token, and `app_id` and `stdio`
   (runtime, package, command, args, the names of its variables, plain and secret, the mount and
   the App's name). Its status is `starting`.

**Reaching it.** Only a connection whose URL is its own App's Service (`mcp_stdio_app_address`)
marks its requests `cluster_service`: the transport then takes plain `http` to that
`<service>.<namespace>.svc.cluster.local` name and its private address, and nothing else private,
so a stdio connection cannot be pointed at Lake's Postgres or a workspace Zetta. `update_mcp_connection`
refuses `url`, `transport` and `auth` on a stdio connection; its name, tools, re-export, agent and
streams change as for any connection.

**Lifecycle.** `refresh_mcp_connection` (and any call) first reads the App from lake's store: until
it is ready the connection stays `starting` and says why (`its App is Pending: …`, `its App is
stopped: start it to use this server`), without calling it; once ready, discovery runs as for any
server. `get_mcp_connection` answers the App's state as `app: {phase, ready, desired, message}`.
The connection's page starts, stops and restarts the App (`start_app`, `stop_app`, `restart_app`)
and shows its logs (`app_logs`): what the package installer and the server print.
`delete_mcp_connection` deletes the App first (`delete_app`, as the person); when it cannot, the
connection stays. The secret stays in its mount.

**Metering.** The App is metered like any App ([metering](metering.md#resources), lake#658): the
Orama operator reports the CPU and RAM its pod is scheduled with, and Lake counts its `cpu`, `ram`
and `instances` as an `app`. Its traffic is Lake's exchanges with it, each counted as the
connection's `calls` and `transfer` (`mcp_out`, `mcp_in`); what the server itself sends to the
internet is App traffic, which the operator does not report yet, as for every App.

**What it needs.** The workspace needs HTTPS egress to the internet (the Lake's
`spec.workspaces.overrides[<workspace>].internetEgress`) for `npx` or `uvx` to install the package
at start and for the server to reach what it serves; without it the App's logs show the install
failing. The App has a read-only root and a 1 GiB `/tmp`, where the image keeps every cache. A Lake
without `LAKE_MCP_RUNNER_IMAGE`, or whose `LAKE_MCP_RUNNER_IMAGE` is not pinned by digest
(`name@sha256:<digest>`, the only image the operator runs an App with; lake#783), answers
`not_supported`.

**The runner** (`oramasearch/lake-mcp-runner`, its `docs/api.md`): one child, initialized once,
whose answer every HTTP session's `initialize` is given; each forwarded request under an id of the
runner's own, so sessions never collide; the child's notifications on the `GET` event stream; a
request from the child (sampling, roots, elicitation) refused, since Lake declares no client
capabilities; a request with an `Origin` refused; the child's stderr as the App's logs. It exits
when the child exits, and the App restarts it.

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
token renewed with a rotated refresh token. `...McpConnectionSignInIsBoundToTheServerItIsFor`
(lake#927): resource metadata naming another API, port, path, query or no resource, and
authorization server metadata naming another issuer or none, stop the sign-in before anything is
registered; the server's URL with a trailing slash, or its origin, is accepted; a connection moved
to another server between beginning and completing a sign-in never exchanges the code, and one
moved after signing in never sends the other server its token, nor its own server once moved
back. `...McpConnectionMovedToAnotherAddressKeepsNoCredential` (lake#928): a bearer connection moved
without naming its secret is refused and the new address hears nothing, while one moved with its
secret named again carries it; an OAuth connection moved to a server that a sign-in is then begun at
sends it neither the refresh token nor a pending sign-in's code and verifier, and after moving back
neither the old sign-in nor the pending one is used. `...McpConnectionStreamsMirrorNotificationsAndDriveTheAgent`:
notifications mirrored to their topic after a subscription, one job per input event as the person,
and its result published once on the output topic.
`...McpConnectionDispatchNamesExportsAndStreamingWorkspacesOnly`: no row for a connection that
neither streams nor re-exports, the endpoint digest changing on export and on an allow-list change
(and not on a refresh that changes nothing), a session that listed not told again, the worker
calling no workspace at all with no streaming connection and only the streaming workspace with
one, and the rows gone when the stream or the connection is. `HttpServer.WorkspaceMcpSessionsLearnThatExportedToolsChanged`
drives `/w/<workspace>/mcp`: the notice after an export until the session lists, again after a
change of what is exported, and a new session on Lake's own list once nothing is.
`Mcp.WorkspaceDigestFollowsExportedToolsAndListedSessionsAreNotToldAgain` checks the digest and the
bounded memory of listed sessions.
`...McpStdioConnectionsRunAsAWorkspaceApp` (lake#659): no runner image is `not_supported`; a bad
runtime, package, command or variable name and `url` beside `stdio` are refused before anything
starts; the runner's secret (token and command) in the person's mount, never answered; the App with
the runner's image, `framework: mcp`, port 8080 and its secret variables; the URL of the App's own
Service, and no other Service accepted; `url`, `transport` and `auth` refused on update; a refresh
before the App is ready says so; deletion deletes the App and keeps the secret.
`McpClient.AWorkspaceAppIsReachedOnlyAtAClusterServiceName` checks the cluster Service rule, and
`McpClient.DrivesAStdioServerThroughTheRunner` (run when `LAKE_MCP_RUNNER_URL` and
`LAKE_MCP_RUNNER_TOKEN` name a running runner) drives the MCP reference server through it with
Lake's own client: discovery, `echo`, a resource read, and a wrong token refused.
`SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls each capability over MCP.
