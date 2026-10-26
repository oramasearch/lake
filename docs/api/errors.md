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
- `operation_result` appears when a completed operation could not be audited; it preserves the original success value or failure envelope. An outer failure does not mean the operation was rolled back.
- `reason` and `help` appear on `unauthenticated` only.
- `warnings`, on a success or a failure, appears while the account the call acted on has a low,
  negative or blocked prepaid balance where billing is on: `[{code, message, customer_id,
  balance_micros, add_micros, add_funds_url}]`, `code` `balance_low`, `balance_negative` or
  `balance_blocked` ([balance](billing.md#the-warning), lake#1114). It is added once, by
  `Registry::call` for the outermost call, so every capability carries it on REST and MCP alike.
- `details` appears on `payment_required` (the balance, the limit and the Add funds link) and on
  `safe_mode_warning`: what the database's safe mode would run and the
  confirmation that runs it ([safe mode](safe-mode.md), lake#906).

## Codes

`error_codes()` in `src/api/registry.cpp` is the table; `rest_status` reads its status from it.

| Code | REST | Meaning |
|---|---|---|
| `invalid_input` | 400 | The input is at fault; `path` points at the field. |
| `secret_required` | 400 | A value that looks like a secret was sent in plain text (a Ship app's environment); store it as a Secret and pass its reference. |
| `invalid_digest` | 400 | An archive or record does not match the digest it carries, so it was changed or damaged. |
| `invalid_signature` | 400 | A signature does not verify against its key. |
| `unauthenticated` | 401 | No credential was accepted. Bearer refusals carry `reason` (`missing`, `invalid` or `expired`) and `help` (`/developer/mcp`). Password/MFA/recovery credential failures and account cooldowns share `invalid credentials`, without account-discovery details. |
| `payment_required` | 402 | The account's prepaid balance is past its overdraft limit, or it has not made its first top-up, and the call starts something that costs money ([balance](billing.md#the-gate), lake#1114). `details` is `{customer_id, balance_micros, overdraft_micros, add_micros, add_funds_url, reason}` (`reason` `first_top_up` or `overdraft`). Reading, signing in, stopping and deleting are never refused this way. |
| `unauthorized` | 403 | The credential is valid but does not grant this call on this object. |
| `mfa_required` | 403 | The account must finish multi-factor verification first. |
| `identity_proof_required` | 403 | An identity document becomes an account's identity only with a fresh signature by its key: sign `begin_identity_proof`'s challenge and send `identity_proof` ([identity.md](identity.md#proving-you-hold-the-key)). |
| `forbidden` | 403 | The object is read-only for everyone: one of Orama's catalogue solutions cannot be edited or deleted ([solutions](solutions.md)); or the entry is off on this Lake for everyone, as Lake's own password sign-up and sign-in are under Clerk ([clerk.md](clerk.md), lake#1098). The message says why and what to do instead, such as `duplicate_solution`. |
| `consent_required` | 403 | The workspace requires an approval before it reaches an outside source, which lake cannot give. |
| `not_found` | 404 | The addressed object does not exist, or the caller cannot see it. |
| `unknown_capability` | 404 | No capability has this name. |
| `method_not_allowed` | 405 | The capability exists but takes the other HTTP method: reads GET, actions POST. Also `GET /mcp`. |
| `conflict` | 409 | The change conflicts with existing state, such as a name already taken. |
| `already_exists` | 409 | A repository object or reference with this identity already exists. |
| `locked` | 409 | Another writer holds the repository reference; retry. |
| `unrecoverable` | 409 | The request cannot succeed as asked, such as recovering a key only a lost device held. |
| `not_approved` | 409 | The workspace action needs an approval it does not have yet: approve the plan or the decision first. |
| `safe_mode_warning` | 409 | The database's safe mode warns on what the call would run. `details` is `{database` or `connection, mode, statements, confirmation}`; the same call repeated with `safe_mode_confirmation` set to `confirmation` runs the statements ([safe mode](safe-mode.md)). |
| `gate_blocked` | 409 | A pipeline gate refused the run; the message names the failing check. |
| `sign_in_required` | 409 | An MCP connection's server needs a person to sign in again, or its credential is missing or refused ([MCP connections](mcp-connections.md#signing-in)). |
| `invalid_chain` | 409 | The audit log's hash chain is broken: an event is missing or was changed. |
| `size_invalid` | 413 | The request body is larger than the limit. |
| `engine_error` | 422 | The workspace engine or the outside service refused the call; the message is its. |
| `training_failed` | 422 | A Quickstart model confirmed in Ask Orama could not be trained: the engine refused to start it, or Lake could not prepare it. The message says why in plain words. `confirm_ask_proposal` answers it as its `code` with `ok: false` ([after a confirm](quickstart.md#after-a-confirm), lake#1204). |
| `not_supported` | 501 | This lake or its workspace engine cannot do what was asked yet. |
| `quota_exceeded` | 507 | The customer's storage quota is full; existing content stays readable. |
| `store_error` | 500 | The server failed; the message says what failed. |
| `corrupt` | 500 | Stored repository data failed an integrity check. |
| `io` | 500 | Repository storage could not be read or written. |
| `crypto_error` | 500 | A signing, hashing or certificate operation failed on the server. |
| `store_unavailable` | 503 | The control-plane store, the workspace or another dependency is not reachable now. |
| `workspace_not_ready` | 503 | The workspace or service exists but is not ready yet. |
| `audit_queue_full` | 503 | The workspace audit queue cannot accept the event; `operation_result` preserves the original operation envelope. Inspect it before retrying a change. See [audit](audit.md#workspace-audit). |
| `busy` | 503 | The resource is busy; retry. |
| `rate_limited` | 429 | A request limit was reached. Authentication callers retry after the fifteen-minute client [cooldown](identity.md#authentication-attempt-limits); waiting-request limits retry when capacity permits. Long-poll capacity refusals send `Retry-After: 1` ([HTTP](../serving/http.md#connections-and-workers), lake#534). |
| `usage_limit_reached` | 429 | The account has used its monthly limit on this dimension (model tokens); an administrator raises or removes it with `set_usage_limit`. The model gateway answers it before it calls the model ([model-gateway.md](model-gateway.md#limits), lake#248). |

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
401 on every capability a visitor may call: lake's own public reads, those whose object is the
server (`viewer`, `server_info`, `list_docs`, `get_doc`; lake#594), and the public reads of
repositories and their content (`list_repositories` and every capability that allows an anonymous
public read; lake#775). A script with a mistyped or revoked key learns so instead of reading a
visitor's answer, such as `signed_in: false` or an empty repository list. A browser's stale
`lake_session` cookie is not refused; it is answered as a visitor, so the web app shows its sign-in
page. Capabilities that verify their own bearer (SCIM providers, sign-in, the agents' calls) keep
their own answers. MCP answers the same error object in a `tools/call` result with
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
  "row 2 has no 'tenure'" at `/rows/2/tenure`, and rewrites the message to count from one with the
  row's key: "row 3 ('partial') has no 'tenure'" (lake#819).
- **A message that leads with an input field's name** (`confirm must be the service name`): the
  registry points `path` at that field when the handler gave none.
- **An account that does not own the workspace** is refused at `/customer_id` (`customer_id is not the
  account that owns workspace …`); an unknown workspace at `/workspace_id`.
- **A database constraint the input broke** (lake#592). PostgreSQL's diagnostic never reaches a
  caller (it is logged as `api_store_error`); `constraint_rules` in `src/api/registry.cpp` turns
  the constraints an input can break into the rule, at the field. The field is the caller's `path`
  when the handler gave one, so `create_drive`'s `repository_name` reads *"repository_name is 1 to
  256 lowercase letters, digits and hyphens, starting and ending with a letter or digit"* at
  `/repository_name`, and otherwise the constraint's own field: an empty `attestation_digest` is
  *"attestation_digest is required: …"* at `/attestation_digest`, and a second identical grant
  *"principal_taap_id already holds this role here: the item is already shared with that person in
  that role"* at `/principal_taap_id` (for `create_grant`; `share_drive_folder` reaches neither:
  lake makes its digest, and sharing again what is already shared answers the existing grant with
  `existing: true`, lake#585; `create_grant`, `create_membership` and `accept_invitation` make
their digest too since lake#602, and refuse a supplied one that is not lake's at
`/attestation_digest`). The in-memory store names the same constraints in its
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
