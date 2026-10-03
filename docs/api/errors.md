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
| `engine_error` | 502 | The workspace engine could not complete the call; the message is its. |
| `not_supported` | 501 | This lake or its workspace engine cannot do what was asked yet. |
| `quota_exceeded` | 507 | The customer's storage quota is full; existing content stays readable. |
| `store_error` | 500 | The server failed; the message says what failed. |
| `corrupt` | 500 | Stored repository data failed an integrity check. |
| `io` | 500 | Repository storage could not be read or written. |
| `crypto_error` | 500 | A signing, hashing or certificate operation failed on the server. |
| `store_unavailable` | 503 | The control-plane store, the workspace or another dependency is not reachable now. |
| `workspace_not_ready` | 503 | The workspace or service exists but is not ready yet. |
| `busy` | 503 | The resource is busy; retry. |

`not_supported` was 500, `engine_error` 500, `quota_exceeded` 500, and a missing or rejected
credential 403 `unauthorized` before lake#450.

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
refuses an anonymous caller. MCP answers the same error object in a `tools/call` result with
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
