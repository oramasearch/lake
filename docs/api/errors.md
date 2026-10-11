# Errors

Every capability answers a failure the same way on REST and MCP:

```json
{ "ok": false, "error": { "code": "invalid_input", "message": "…", "path": "/rows/2/tenure" },
  "duration_ms": 0.4 }
```

- `code` is one of the codes below, and only those. `ApiResult::failure` (`src/api/registry.cpp`)
  maps the workspace engine's and liquid's own spellings onto them (`invalid_argument` →
  `invalid_input`, `unsupported` → `not_supported`, `unavailable` → `store_unavailable`,
  `internal` → `store_error`). Any other code is answered as `store_error`, with the original code
  at the start of the message, so a caller never meets an undocumented code. The OpenAPI
  `error_envelope` schema lists the same codes as an `enum`.
- `path` is a JSON pointer to the input field at fault whenever the input is at fault, and empty
  otherwise. It is never put in the message. See [Where `path` comes from](#where-path-comes-from).
- `reason` and `help` appear on `unauthenticated` only.

## Codes

`error_codes()` in `src/api/registry.cpp` is the table; `rest_status` reads its status from it.

| Code | REST | Meaning |
|---|---|---|
| `invalid_input` | 400 | The input is at fault; `path` points at the field. |
| `secret_required` | 400 | A value that looks like a secret was sent in plain text (a Ship app's environment); store it as a Secret and pass its reference. |
| `invalid_digest` | 400 | An archive or record does not match the digest it carries, so it was changed or damaged. |
| `invalid_signature` | 400 | A signature does not verify against its key. |
| `unauthenticated` | 401 | No credential was accepted: `reason` says whether it was `missing`, `invalid` or `expired`, and `help` links to the page that makes a new key (`/developer/mcp`). |
| `unauthorized` | 403 | The credential is valid but does not grant this call on this object. |
| `mfa_required` | 403 | The account must finish multi-factor verification first. |
| `consent_required` | 403 | The workspace requires an approval before it reaches an outside source, which lake cannot give. |
| `not_found` | 404 | The addressed object does not exist, or the caller cannot see it. |
| `unknown_capability` | 404 | No capability has this name. |
| `method_not_allowed` | 405 | The capability exists but takes the other HTTP method: reads GET, actions POST. Also `GET /mcp`. |
| `conflict` | 409 | The change conflicts with existing state, such as a name already taken. |
| `already_exists` | 409 | A repository object or reference with this identity already exists. |
| `locked` | 409 | Another writer holds the repository reference; retry. |
| `unrecoverable` | 409 | The request cannot succeed as asked, such as recovering a key only a lost device held. |
| `not_approved` | 409 | The workspace action needs an approval it does not have yet: approve the plan or the decision first. |
| `gate_blocked` | 409 | A pipeline gate refused the run; the message names the failing check. |
| `invalid_chain` | 409 | The audit log's hash chain is broken: an event is missing or was changed. |
| `size_invalid` | 413 | The request body is larger than the limit. |
| `engine_error` | 422 | The workspace engine or the outside service refused the call; the message is its. |
| `not_supported` | 501 | This lake or its workspace engine cannot do what was asked yet. |
| `quota_exceeded` | 507 | The customer's storage quota is full; existing content stays readable. |
| `store_error` | 500 | The server failed; the message says what failed. |
| `corrupt` | 500 | Stored repository data failed an integrity check. |
| `io` | 500 | Repository storage could not be read or written. |
| `crypto_error` | 500 | A signing, hashing or certificate operation failed on the server. |
| `store_unavailable` | 503 | The control-plane store, the workspace or another dependency is not reachable now. |
| `workspace_not_ready` | 503 | The workspace or service exists but is not ready yet. |
| `busy` | 503 | The resource is busy; retry. |
| `rate_limited` | 429 | Too many requests from this caller, or in all, are waiting; retry. Sent with `Retry-After: 1` when a long poll finds the long-request caps full ([http](../serving/http.md#connections-and-workers), lake#534). |

`not_supported` was 500, `engine_error` 500, `quota_exceeded` 500, and a missing or rejected
credential 403 `unauthorized` before lake#450. `engine_error` was 502 from lake#450 until lake#572.

### No code answers 502 or 504

lake is reached through Cloudflare, which answers an origin's `502` or `504` with its own HTML page
("Error 502: Bad gateway … the origin web server returned an invalid or incomplete response"), so a
REST caller never saw lake's JSON body and could not tell a typo in SQL from an outage (lake#572).
Cloudflare passes `500`, `501` and `503` bodies through unchanged, and every 4xx. So:

- `engine_error` is `422`: the workspace engine refused the call (`table "x" does not exist`, a SQL
  parse error) or an outside service refused it (OpenAI could not transcribe the audio). The caller
  can change the input; it is not lake's fault.
- An engine that cannot be reached is `store_unavailable` (`503`); lake's own faults stay `500`.
- An identity application's OAuth callback answers `503` when the provider's token or user
  endpoint fails.

`ApiErrors.NoCodeAnswersAStatusCloudflareReplaces` (`tests/test_api.cpp`) fails if a documented code
is given `502` or `504`. See [http](../serving/http.md#statuses-behind-cloudflare).

## 401: missing, invalid or expired

A capability that needs a credential, called without one that lake accepts, answers 401
`unauthenticated` with a `WWW-Authenticate` challenge (RFC 6750):

```text
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer realm="lake", error="invalid_token",
  error_description="the credential has expired", error_uri="https://<host>/developer/mcp"

{"ok":false,"error":{"code":"unauthenticated","reason":"expired",
 "message":"the credential has expired; sign in again or make a new key",
 "help":"https://<host>/developer/mcp","path":""},"duration_ms":0.0}
```

- `missing`: no bearer and no session cookie.
- `expired`: the secret is a real session or token whose `expires_at` has passed. Both stores
  check the secret first, so only a caller holding the real secret learns that it expired.
- `invalid`: anything else: unknown, mistyped, revoked, or a revoked device.

A capability that lets anonymous callers read public repositories answers the same 401 when it
refuses an anonymous caller. A **bearer** key lake rejects (invalid or expired) is refused with this
401 on lake's own public reads too, those whose object is the server (`viewer`, `server_info`,
`list_docs`, `get_doc`; lake#594): a script with a mistyped key learns so instead of reading `signed_in: false`. A
browser's stale `lake_session` cookie is not refused there; `viewer` answers it as a visitor, so
the web app shows its sign-in page. Capabilities that verify their own bearer (the agent bus, SCIM
providers) keep their own answers. MCP answers the same error object in a `tools/call` result with
`isError: true` over HTTP 200, as it does every capability failure. A credential that is valid but
lacks a scope or a grant is still 403 `unauthorized`.

## Where `path` comes from

- **Schema validation** (`validate_input`): every type, enum, bound, required and unexpected field.
- **The handler**, which names the field it refused.
- **Workspace engine refusals** (`workspace_failure` in `src/api/workspace_capabilities.cpp`), for
  `workspace_query`, `workspace_act`, `predict`, `search_index`, `get_model`, `get_dataset` and the
  experiment capabilities: the engine's own `path` when it gives one (zetta#1217), in the caller's
  name for the field; otherwise "required property 'x'" is about `x`; a message that leads with a
  pointer (`/format: instance not found in required enum`) has the pointer moved to `path` and the
  message becomes `format: instance not found in required enum`; a message that quotes a value
  the caller sent (`no model named 'm-1'`) is about the field that carried it. An `engine_error`
  or `invalid_argument` whose message names a field is answered as `invalid_input`. Inside
  `workspace_query`/`workspace_act` the path is below `/input`. `predict` reports the engine's
  "row 2 has no 'tenure'" at `/rows/2/tenure`.
- **A message that leads with an input field's name** (`confirm must be the service name`): the
  registry points `path` at that field when the handler gave none.
- **A space that does not own the workspace** is refused at `/customer_id` (`customer_id is not the
  space that owns workspace …`); an unknown workspace at `/workspace_id`.
- **A database constraint the input broke** (lake#592). PostgreSQL's diagnostic never reaches a
  caller (it is logged as `api_store_error`); `constraint_rules` in `src/api/registry.cpp` turns
  the constraints an input can break into the rule, at the field. The field is the caller's `path`
  when the handler gave one, so `create_drive`'s `repository_name` reads *"repository_name is 1 to
  256 lowercase letters, digits and hyphens, starting and ending with a letter or digit"* at
  `/repository_name`, and otherwise the constraint's own field: an empty `attestation_digest` is
  *"attestation_digest is required: …"* at `/attestation_digest`, and a second identical grant
  *"principal_taap_id already holds this role here: the item is already shared with that person in
  that role"* at `/principal_taap_id`. The in-memory store names the same constraints in its
  messages. Only a diagnostic for a constraint not in the table still answers the generic *"the
  request contains invalid data"*; add the constraint to the table when an input can reach it.
- **Secrets** name the secret and the field: *"no secret named nope"* at `/name`, *"secret database
  has no version 9"* at `/version` (or `/versions` for the version changes), *"version 1 of secret
  database is deleted; undelete it with undelete_secret_versions"*. A mount, key or derived key name
  states the handle rule (`control::handle_rule`).

## Other HTTP answers

These are not capability results but use the same envelope:

- `GET /v1/<name>` for an action, or `POST` for a read: 405 `method_not_allowed`, naming the method.
- An unknown route: 404 `not_found`.
- A JSON body over 64 MiB: 413 `size_invalid`; a body that is not JSON: 400 `invalid_input`.
- An empty query key (`/v1/server_info?`, `?&limit=5`) is ignored; it never becomes a field.

## Verify

`ctest --test-dir build -R 'ApiErrors|ApiNaming|ApiDescriptions'` runs the tests in
`tests/test_api.cpp`; `Workspaces.Naming*` and `Workspaces.Sql*` in `tests/test_workspaces.cpp`
cover the workspace side, and `PostgresSessions.AnExpiredCredentialIsToldApartFromAnUnknownOne`
the Postgres store.
`ApiErrors.EveryReturnedCodeIsDocumented` scans `src/` for every error code literal and fails if
one is missing from `error_codes()` or from the table above.
