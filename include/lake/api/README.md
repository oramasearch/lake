# Public API types

`ExperimentsPage` contains capability values and resolved display permissions for the pure
study/run/comparison renderer. Experiment asset declarations expose embedded bytes and hashed
URLs without filesystem or HTTP dependencies. See [the UI contract](../../../docs/ui/experiments.md)
and [registered API operations](../../../docs/api/experiments.md).

`Capability::post_alias` gives read capabilities a JSON POST route as well as GET, preserving
read policy and identical OpenAPI/MCP schemas. `StreamsPage` holds only capability values and
resolved UI permissions, including list cursors and Agent event filters; `web_ui_streams` is a
pure renderer. See `docs/api/streams.md` and `docs/ui/streams.md` for the contracts.

`ServerOptions::external_origin` is trusted instance configuration for repository terminal
instructions. The HTTP adapter validates it and supplies it to `RepositoryHomeData`. It takes
precedence over `https://` plus the trusted identity/platform `public_host`; with neither value,
commands are omitted. See
[configuration.md](../../../docs/serving/configuration.md) and
[forge.md](../../../docs/ui/forge.md).

`ApiContext` carries trusted issuer session ID, kind, scopes and expiry from the common credential
resolver. Token request JSON cannot supply those fields. Keep HTTP and PostgreSQL types out of
these public headers.

Verify changes with the full PostgreSQL-backed static and shared CTest suites. Keep the identity
contract in `docs/api/identity.md` updated with changes to token derivation.

The registry owns the response envelope and its success/error schemas. REST and MCP use that
shared contract, and input validation recursively checks object fields and array items with JSON
pointer errors. See [`docs/api/README.md`](../../../docs/api/README.md) for the envelope decision
and HTTP conformance proof.

`WebUiViewer` is presentation state, passed explicitly to shared-shell page renderers. Its
operator flag comes from the resolved credential and does not authorize a capability. Boolean
renderer arguments remain compatible and mean an ordinary account, never an operator. See
[front-door.md](../../../docs/ui/front-door.md).

`AppsPage` carries the selected ApplicationAgent task, working state, and exact proposal digest to
the pure Ship Apps renderer. Authorization and digest verification remain in the HTTP adapter.
