# REST API and OpenAPI

Everything you can do in Lake's web app is also a REST call and an MCP tool. One capability
registry defines each operation once: its name, its input and output schemas, who may call it and
what it records in the audit log. REST under `/v1`, the OpenAPI document and the MCP server are
three renderings of that registry, so they accept the same input, check the same permissions and
answer the same errors. The web app itself calls nothing else.

This page covers the REST API and OpenAPI. [MCP Setup](/docs/mcp) connects an AI agent, and
[The liquid CLI](/docs/cli) pushes and pulls repositories.

## The Envelope

Every answer is JSON in one of two shapes. Success:

```json
{"ok": true, "value": {"name": "lake"}, "duration_ms": 0.4}
```

Failure:

```json
{"ok": false, "error": {"code": "invalid_input", "message": "workspace_id is required", "path": "/workspace_id"}, "duration_ms": 0.1}
```

- `value` is the capability's own result; its schema is in OpenAPI and on **Developer → API**.
- `code` is always one of the codes in [Error Codes](#error-codes), and the HTTP status follows it.
- `path` is a JSON pointer to the input field at fault whenever your input is at fault, and empty
  otherwise. Show it next to the field.
- `duration_ms` is how long Lake took.

## Concepts

| Term | What it is |
|---|---|
| **Capability** | One operation, such as `list_repositories` or `create_workspace`. Its name is the REST path, the MCP tool name and the OpenAPI `operationId`. |
| **Read** | A capability that changes nothing. Call it with `GET`, its input in the query string. |
| **Action** | A capability that changes something (or, like `predict`, takes a body). Call it with `POST` and a JSON body. |
| **Key** | A Lake API token: `Authorization: Bearer <key>`. It acts as you, limited to the capabilities it names. |
| **`customer_id`** | The id of the account that owns your workspaces (yours, or your organization's). |
| **`workspace_id`** | The workspace a call is about. Design, Train, Ship, Transcripts and Ask Orama calls require it; Lake never picks one for you. |

## Make Your First Call

1. Open **Developer → API**. The strip at the top shows the base URL (`<origin>/v1`) with **Copy**.
2. On the **Keys** tab, choose what the key may do (**Every operation**, **Read-only** or **Named
   capabilities**) and a lifetime, then press **Create an API key**. The key is shown once, under
   *Your New Key: Copy It Now*; Lake keeps only its hash. Store it, then press **I've stored it**.
3. Open the **Quick start** tab. Its blocks are filled in with your origin and, until you leave the
   page, your new key: who you are, a list read, an action, a call inside a workspace, and the
   same from JavaScript.
4. Run the first block in a terminal:

```
curl -s https://lake.oramadev.net/v1/viewer -H "Authorization: Bearer $LAKE_KEY"
```

`viewer` needs no key; with one it names you and the workspaces you can open. Replace
`lake.oramadev.net` with your Lake's address wherever it appears on this page.

### Send a User-Agent

Every call must carry a `User-Agent` header that names your program. `curl`, Python `requests`,
Node's `fetch` and browsers send one already. Python's built-in `urllib` sends
`Python-urllib/3.x`, which the network edge in front of lake.oramadev.net refuses before the call
reaches Lake: the answer is `403` with the plain-text body `error code: 1010`, not Lake's JSON
envelope. Name your script instead:

```
import json, os, urllib.request

req = urllib.request.Request(
    "https://lake.oramadev.net/v1/viewer",
    headers={"Authorization": "Bearer " + os.environ["LAKE_KEY"],
             "User-Agent": "my-script/1.0"},
)
print(json.load(urllib.request.urlopen(req))["value"])
```

The **Quick start** tab on **Developer → API** shows the same call from Python.

## Reads and Actions

A read is `GET /v1/<capability>` with its input as query parameters. Numbers and booleans are
read as the schema declares them:

```
curl -s "https://lake.oramadev.net/v1/list_repositories?limit=5" -H "Authorization: Bearer $LAKE_KEY"
```

An action is `POST /v1/<capability>` with a JSON body and `Content-Type: application/json`. For
example, `POST /v1/create_repository`:

```
curl -s https://lake.oramadev.net/v1/create_repository \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "analytics", "description": "Our analysis code"}'
```

Calling a read with `POST` or an action with `GET` answers `405 method_not_allowed`. A field the
schema does not declare is refused with `invalid_input` naming it (`unexpected field …`), so a typo
never passes silently.

## Find Your Ids

- **`workspace_id`**: in the address bar of any Design, Train or Ship page, `/w/<workspace id>/…`;
  on **Ship → Workspaces**; or from `GET /v1/list_workspaces?customer_id=<customer id>`.
- **`customer_id`**: `viewer` lists the accounts you act for and their workspaces. On a page of an
  organization, its id is in the address.
- Other ids (a repository, a run, a model) are on their pages and in the list call that returns
  them. One concept has one name across every capability: `run_id`, `model_id`, `index_id`. Older
  spellings (`run`, `model`, `index`) are still accepted and marked deprecated in the schema.

## The Workspace Prefix

Any REST call, the OpenAPI document and MCP also answer under `/w/<workspace id>/`. For a
capability whose schema has `workspace_id`, the prefix fills it in when your input leaves it out;
if your input names a different workspace, the call is refused with `invalid_input`. A malformed,
unknown or deleted workspace in the prefix answers `404 not_found`.

A list that can be narrowed to a workspace is narrowed by the prefix. This lists only the
repositories of one workspace:

```
curl -s "https://lake.oramadev.net/w/<workspace id>/v1/list_repositories?limit=5" -H "Authorization: Bearer $LAKE_KEY"
```

The prefix is how the web app scopes every call a `/w/<workspace id>/` page makes. A client you
give a base URL ending in `/w/<workspace id>` works in that workspace without repeating it.

## Inside a Workspace's Engine

Each workspace has its own engine. Every operation of the engine a Lake caller may use is its own
capability, with its own route, input schema and MCP tool: `GET /v1/list_projects`,
`POST /v1/create_project`, `POST /v1/check_pipeline`, `POST /v1/start_run`,
`GET /v1/get_run_report`, `GET /v1/list_models`, `POST /v1/create_agent` and the rest. Each takes
`customer_id`, `workspace_id` and the engine's own fields, for example `POST /v1/create_project`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "retention"}
```

A few have Lake's name where the engine's would be unclear or is taken: the engine's `gate` is
`check_pipeline`, `run` is `start_run`, `search` is `search_workspace_index`, `chat` is
`chat_with_agent`. The OpenAPI document groups them (Projects and pipelines, Training and runs,
Models, Search indexes, Datasets, Connections, Agents, Decisions, Engine).

Two older capabilities reach the same operations by the engine's names, each through an
allow-list, with the same permission checks and audit as every other call:

- `workspace_query` runs one allowed engine **read**: projects, pipelines (`get_graph`), datasets,
  runs and their reports, models, indexes, solutions, methodologies, jobs and traces. It is a
  `POST` because its input is nested, but it changes nothing.
- `workspace_act` runs one allowed engine **action**: create a project, approve and run a plan,
  upload a dataset, publish a model or an index, and the rest of what Design, Train and Ship do.

Both take `customer_id`, `workspace_id`, the engine `capability` (OpenAPI lists the names each
takes) and its `input`. For example,
`POST /v1/workspace_query`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "list_projects", "input": {}}
```

and `POST /v1/workspace_act`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_project", "input": {"name": "retention"}}
```

An engine capability off the lists is refused with `invalid_input`, so nothing reaches the engine's
administrative operations. An engine refusal keeps its field: its `path` is below `/input`.
Engine reads give up after 15 seconds and actions after 60.

Read-only SQL over a workspace's data is its own capability, `POST /v1/workspace_sql`, which takes
one `SELECT` or `WITH` statement:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "statement": "SELECT * FROM accounts LIMIT 20"}
```

## OpenAPI

The full contract is `GET /v1/openapi.json` (OpenAPI 3.1): every operation, with its input schema
(query parameters for a read, a request body for an action) and its success schema, the envelope
around the capability's value. It needs no key. Load it into Postman, Insomnia or any OpenAPI code
generator.

On **Developer → API**:

- **openapi.json** in the header downloads it, pretty-printed.
- The **Operations** tab lists every operation as your session receives it, grouped by product
  area, searchable by path, name, summary or group, and filtered by read or write. Each opens to
  its description, the permission it needs, its parameters, its request schema, the schema of
  `value` and a **Copy as curl** block with the required fields as placeholders.
- `workspace_query` shows as a read: it is marked `x-lake-action: read`.

The document under `/w/<workspace id>/v1/openapi.json` is the same document.

## Keys and Authentication

Send `Authorization: Bearer <key>` on every call. A key acts as the person who made it and can
never do more than they can:

- **What it may call.** Every operation, read-only operations, or capabilities you name. For liquid
  push and pull, the **Named capabilities** field fills in the set in one click.
- **How long it lasts.** As long as your sign-in allows (at most 30 days), 1 day, 7 days, a number of
  seconds, or **Never** (a standing key, which needs a reason, named capabilities and the opt-in of the account that
  owns your workspaces).
- **Revoking** a key on the **Keys** tab (or on **Developer → MCP**, or **Tokens** in the account menu) stops
  it on its next call. Revoking the sign-in that made it revokes it too.

The keys are listed with their access, expiry, last use and state. [Tokens and Keys](/docs/tokens)
has the full rules and the REST calls that make and revoke keys.

A few reads need no key: `server_info`, `viewer`, the Docs (`list_docs`, `get_doc`), public
repositories, the transparency monitor, and `GET /v1/openapi.json`.

## Lists and Paging

Most lists take `limit`. Where a list pages, its answer says how to continue:

- `list_transcripts`, `list_streams` and `list_artifacts` answer `next_cursor` while more follow;
  pass it back as `cursor`.
- `audit` and `my_audit` continue after the last event with `after_id`.

Each capability's page in [Docs](/docs) and its OpenAPI operation name the inputs it takes.

## Error Codes

| Code | HTTP | Meaning |
|---|---|---|
| `invalid_input` | 400 | Your input is at fault; `path` points at the field. |
| `secret_required` | 400 | A value that looks like a secret was sent in plain text; store it as a Secret and pass its reference. |
| `invalid_digest`, `invalid_signature` | 400 | An archive, record or signature does not verify. |
| `unauthenticated` | 401 | No credential was accepted. See [When a Key Is Refused](#when-a-key-is-refused). |
| `unauthorized` | 403 | The credential is valid but does not grant this call on this object. |
| `mfa_required` | 403 | Finish multi-factor verification first. |
| `consent_required` | 403 | The workspace requires an approval Lake cannot give. |
| `not_found` | 404 | The object does not exist, or you cannot see it. |
| `unknown_capability` | 404 | No capability has this name. |
| `method_not_allowed` | 405 | Reads are `GET`, actions `POST`. |
| `conflict`, `already_exists` | 409 | The change conflicts with existing state, such as a name already taken. |
| `locked` | 409 | Another writer holds the repository reference; retry. |
| `not_approved` | 409 | The workspace action needs an approval first: approve the plan or the decision. |
| `gate_blocked` | 409 | A pipeline check refused the run; the message names the check. |
| `unrecoverable` | 409 | The request cannot succeed as asked. |
| `invalid_chain` | 409 | An audit log's hash chain is broken. |
| `size_invalid` | 413 | The request body or a value in it is over its limit. |
| `rate_limited` | 429 | Too many long requests are waiting; retry after `Retry-After`. |
| `store_error`, `corrupt`, `io`, `crypto_error` | 500 | The server failed; the message says what. |
| `not_supported` | 501 | This Lake or its workspace engine cannot do this yet. |
| `engine_error` | 422 | The workspace engine or an outside service refused the call (a SQL typo, unreadable audio); the message is its own. |
| `store_unavailable`, `busy` | 503 | A dependency is unreachable or busy; retry. |
| `workspace_not_ready` | 503 | The workspace exists but is not ready. |
| `quota_exceeded` | 507 | Your storage quota is full; what is stored stays readable. |

Any other failure is answered as `store_error`, so you never meet an undocumented code.

## When a Key Is Refused

A call that needs a credential and gets none Lake accepts answers `401 unauthenticated` with a
`WWW-Authenticate: Bearer` challenge, a `reason` and a `help` link to **Developer → MCP**:

- `missing`: no `Authorization` header and no session cookie.
- `expired`: a real key or session whose lifetime has passed. Make a new key.
- `invalid`: anything else: unknown, mistyped, revoked, or from a revoked device.

A valid key that lacks the capability, or a person without the permission, gets `403 unauthorized`
instead. Lake tells `expired` apart only once the secret itself matches, so a guess learns nothing.

## Limits

- **Body size.** A JSON body is at most 64 MiB; a larger one answers `413 size_invalid`. Uploads that
  travel as base64 inside JSON grow by a third.
- **Long requests.** Calls that wait for events (`browse_stream`, `tail_experiment_events` and the
  other long polls) are capped at 64 per caller and 512 in all. Past a cap the call answers `429
  rate_limited` with `Retry-After: 1`. A server that is out of request workers answers `503 busy`.
- **Workspace engine.** Reads time out after 15 seconds and actions after 60.

## Server and Agent Documents

- `GET /v1/server_info` answers Lake's name, version and source revision, with no key.
- `/llms.txt` describes Lake in one page for an AI agent: what it is, how to connect, every tool by
  group and the rules an agent keeps. `/llms-full.txt` adds each tool's description and input
  schema. Both are public and generated from the same registry, so they list exactly the tools the
  server has.
- The Lake skill (`/skills/lake/SKILL.md`) teaches an agent the same; [MCP Setup](/docs/mcp#teach-your-agent)
  installs it.

## Calling From a Browser on Another Site

Bearer calls work from any origin. Lake's browser session cookie does not: a `POST` that only the
`lake_session` cookie authenticates must come from Lake's own origin and be `application/json`, or
it is refused with `unauthorized` (*Cross-site requests cannot use the browser session.*). A script
or app outside Lake therefore always sends a key. [Security Model](/docs/security#cross-site-requests)
says why.

## Troubleshooting

- **`workspace_id is required`**: Design, Train and Ship calls need it; see
  [Find Your Ids](#find-your-ids), or call under the [workspace prefix](#the-workspace-prefix).
- **`customer_id is not the space that owns workspace …`** (at `/customer_id`): the workspace
  belongs to another account. Use the `customer_id` that `list_workspaces` lists it under.
- **`unexpected field …`**: the input has a field the schema does not declare. Check the spelling
  against the operation on **Developer → API → Operations**.
- **`405 method_not_allowed`**: a read was sent as `POST` or an action as `GET`.
- **`403` with the body `error code: 1010`**, not JSON: the edge refused your program's
  `User-Agent` (Python's `urllib` default). Send your own; see
  [Send a User-Agent](#send-a-user-agent).
- **A `502` or `504` HTML page**: it came from the edge, not from Lake, which never answers either
  (an engine refusal is `422 engine_error` with Lake's message). Lake was unreachable or the call ran
  past the edge's 100-second limit; retry, and split a very long recording.
- **`workspace_not_ready`**: the workspace is starting. See
  [Workspaces](/docs/workspaces).
- More answers: [FAQ and Troubleshooting](/docs/faq).

## Related Pages

- [MCP Setup](/docs/mcp)
- [Tokens and Keys](/docs/tokens)
- [The liquid CLI](/docs/cli)
- [Security Model](/docs/security)
- [Workspaces](/docs/workspaces)
- [FAQ and Troubleshooting](/docs/faq)
