# Audit API

lake records signed, hash-linked audit events for every capability call, including authorization
and input failures, a request body lake cannot read among them
([Every call, an unreadable body included](#every-call-an-unreadable-body-included)). Since decision 52 (lake#317) the events are kept in two kinds of log: the
central log in lake's PostgreSQL for control-plane and identity events, and each workspace's own
audit, in its Zetta, for events about that workspace's data. See [Which log](#which-log). The audit stream is the source of evidence for authentication, session and
token lifecycle, denied authorization, grant changes, secrets and certificate activity, retention
refusals, and administrative activity. The canonical event contract is in
[`../design/phase-6.md`](../design/phase-6.md#audit-decision-13).

The current admin sign-in records `admin_token` as its method and the HTTP User-Agent as its
device value when one is supplied. Identity-core credential and registered-device events use the
same fields through `audit::record`.

## Capabilities

All audit capabilities except `identity` require the normal lake bearer or session authentication.

| Capability | REST route | What it returns |
|---|---|---|
| `identity` | `GET /v1/identity` | Lake's public TAAP/1 Ed25519 identity document. |
| `rotate_identity` | `POST /v1/rotate_identity` | A new public identity document after a dual-signed rotation. |
| `audit` | `GET /v1/audit` | `{items: [event], unread_workspaces}` filtered by `principal`, `object`, `outcome`, `from`, `to`, and `limit`, from the central log and the workspaces' audits; `workspace_id`, `customer_id` and `log` choose the logs ([Reading both logs](#reading-both-logs)). An account's administrator reads its own account's ([Who may read it](#who-may-read-it)). |
| `export_audit` | `GET /v1/export_audit` | `{jsonl}` containing the matching event objects, one JSON line each: the central log (with `customer_id`, the events attributed to that account), or with `workspace_id` that workspace's whole audit. |
| `verify_audit` | `GET /v1/verify_audit` | `{verified: true, log}` after checking every stored signature, digest, and predecessor link of the central chain, or with `workspace_id` of that workspace's chain. |
| `set_audit_retention` | `POST /v1/set_audit_retention` | The customer ID and retained days after setting the central log's retention to at least 400 days, and `workspace_audit_retention`. |

`rotate_identity` is an administrative action and produces one audit event through the registry.
Identity history remains in `lake_identity`, so events signed before a rotation remain verifiable.
Passkey registration uses that same identity history to sign a TAAP/1 `passkey_binding/1`
attestation over the credential ID, pool and user's TAAP identity. `list_devices` returns the
stored binding, and verification accepts its historical lake signing identity after rotation.

## Page

`/audit` is the web app's audit page (`frontend/src/pages/admin/AuditPage.tsx`), the operator's:
the whole log, with the same principal, object, outcome, and time filters as `audit`, calling only
`audit`. Opened by an account it says, inside the page, that it is the operator's.

An organization's settings have an **Audit** tab (`/organization?id=<id>&tab=audit`), the
account's surface: it reads `audit {customer_id}` for the organization's account (the central
events attributed to it and its workspaces' audits), filters them by outcome and text, and exports
them through `export_audit`. A person who is not the organization's administrator sees the
refusal in the tab.

## Identity key custody

`LAKE_IDENTITY_SEED` is an optional base64-encoded, 32-byte Ed25519 seed supplied by the
operator. Lake uses that Secret directly and does not copy it into the control database. With no
configured seed, lake generates an identity at server start. The generated seed is AES-256-GCM
wrapped with the customer-independent `LAKE_INSTANCE_KEK` and stored in `lake_identity`; a seed
is never included in an API response or audit event. A Postgres-backed lake requires
`LAKE_INSTANCE_KEK` before it can create or load a generated identity. The in-memory store uses a
process-local development key because it has no persistent state.

`rotate_identity` creates a generated successor and therefore requires `LAKE_INSTANCE_KEK`, even
when the current identity was operator-supplied.

## Retention

Each customer may increase its retention through `set_audit_retention`; a setting can never be
reduced. 400 days is the enforced floor. A request below that floor or below the customer's
existing setting is refused and produces the one `retention` event for that capability call.
Pruning is intentionally absent: the database append-only trigger rejects update and delete
operations on `audit_events`.

The setting stays in `audit_retention` and governs the central log (lake#318): it is the account's
policy, a control-plane row, and the central log is where its events are. A workspace's audit is
the collection `lake_audit_events` of its Zetta, whose `retention_ms` is 0: it is kept until the
workspace is deleted, which is never shorter than any setting, and is deleted with the workspace.
`set_audit_retention` answers `workspace_audit_retention: "kept until the workspace is deleted"`.
A finite workspace retention would be a new collection with a `retention_ms` at or above the
account's setting, copied into (a collection's retention cannot change).

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

For PostgreSQL integration coverage, start Postgres 16, apply `db/schema.sql`, and provide its
URI as `LAKE_TEST_DATABASE`; the `PostgresAudit.RegistryEventsPersistAndVerify` test then runs.
The CI workflow provisions this database automatically.

## Change

Add a new event kind without changing the meaning of an existing field. A canonical payload
change requires a new event version, matching verifier support, and an update to the Phase 6
event contract. New callers must use `audit::record` rather than writing `audit_events` directly.

The audit implementation is compiled once into `lake-core`. Control-store implementations depend
on that library for the provider-neutral audit API; they do not include its implementation source.
This keeps the static-library link graph valid on ELF linkers as well as macOS.

## Exact requests and complete traversal

`audit` and `export_audit` accept `request_id` for exact equality against the recorded request.
Combine it with the existing object/outcome filters to ask for a particular operation; the
filters apply before `limit`. `after_id` continues strictly after a returned event's UUID in
append order. It composes with every filter and does not depend on timestamp uniqueness. A
missing cursor returns no rows. Continue from the last returned ID to traverse a history. For
a finite snapshot through the registry, hold `to` fixed: audit queries themselves append events.
The same fields are available on `audit::Filter` in both stores. The test helper reads the store
directly, so it can exhaust pages until empty without generating further audit events.

The regression in `tests/test_audit_isolation.cpp` places a target after 1,105 same-object events,
queries its request through the registry, and verifies the complete signed chain across pages.
See [testing.md](../build/testing.md) for fixture ownership and the bounded-read sweep.

## An account's own events

`my_audit` takes the same filters as `audit` but always returns only the caller's own events: the
principal is forced to the signed-in TAAP identity, and a `principal` filter cannot widen it. It is
authorized on the caller's own principal object, so any signed-in account may call it. `audit`
reads the whole instance only for the operator; an account's administrator reads that account's
([Who may read it](#who-may-read-it)). The `/activity` page (Organization in the sidebar) renders
it.

## Who may read it

lake#18. `audit` and `export_audit` decide whose audit a call reads before it runs
(`audit_log_object` in `src/api/capabilities.cpp`):

- **No account named**: the whole instance's log, the central log and every workspace's audit.
  Only the operator may read it.
- **An account named**, by `customer_id`, or through `workspace_id` (the workspace's account): that
  account's audit, for the operator and for a principal with `admin` on the account, such as an
  organization's administrator or the owner of a personal account. It is the central events
  attributed to the account (`audit::Filter::customer_id`) and its workspaces' audits, or with
  `workspace_id` that one workspace's audit. A member who may only read the account is refused.

Every other call is one refusal, `unauthorized`, whether the account belongs to someone else or
does not exist, so a guessed id learns nothing; filters (`principal`, `object`, `request_id`) only
narrow the account's events and never reach another's. The refusal is itself an audit event.
`verify_audit` stays the operator's.

An event is attributed to an account (`customer_id` on the event) when the capability names it
itself, else by the workspace the call acted on, the `customer_id` or `organization_id` of its
input, or the organization its event names (`audit_customer_of` in `src/api/registry.cpp`). A
refused call is attributed the same way, so an attempt on an account is evidence in that account's
audit; a name that is not an account's is left unattributed. Events recorded before this change
carry the attribution their capability gave them, often none, and so appear only in the
operator's view.

`tests/test_audit_isolation.cpp` (`AnAccountAdministratorReadsOnlyItsOwnAccountsAudit`, both
stores, both dispatches) covers the administrator's read and export, the refusals of another
account, the whole log, a guessed account and a guessed request, and a reader who is not an
administrator. `tests/test_http_server.cpp`
(`AnAccountAdministratorReadsItsAuditOverRestAndMcp`) covers REST and MCP.

## Every call, an unreadable body included

A request to a capability route whose body lake cannot read (not JSON, too large, cut off, or
for a workspace's Zetta or SCIM not a JSON object) is answered as before, 400 or 413, and recorded
as one `failure` event of that capability (`Registry::refuse`): the caller as principal
(`anonymous` without a credential), the request id, `details.error_code`, `details.input`
(`not_json`, `not_an_object`, `too_large`, `unreadable` or `empty`) and `details.body_bytes`.
The body itself is never recorded. This covers `/v1/<capability>`, a workspace's Zetta
(`workspace_query`/`workspace_act`, with `details.capability`), SCIM and the model gateway
(`model_completion`). If the event cannot be recorded the call answers `store_error`, as any call
does. `tests/test_http_server.cpp` (`AMalformedBodyIsAuditedLikeAnyFailedCall`) checks one signed
event for a well-formed and for a malformed body, without the body's contents.

## Reading the log as a person (lake#445)

- **Order.** `audit` and `my_audit` return the newest matching events first. `order: "oldest"`
  returns append order; a query with `after_id` continues in append order unless it names an
  order. `export_audit`, `verify_audit` and `audit::Filter`'s default keep append order, the
  order the chain verifies in. `audit::Filter::newest_first` selects the newest-first order in
  both stores.
- **Who acted.** Each returned event carries `principal_name`, beside the signed `principal`: the
  person's display name (else email) or the organization's name for a TAAP identity, a library
  agent's name for `agent-service:<agent>`, `Lake operator` for `admin`, and a plain description
  for app, build, share-link and SSO-provider credentials. An identity lake cannot name keeps its
  recorded form. Agent task, launch and schedule credentials act as the person who started the
  agent, so the registry records the agent in the event's `details.acting_agent`, and the name
  reads `Project Agent for Ada Lovelace`. `Store::principal_names` resolves the identities in one
  query per page.
- **What a workspace call did.** `workspace_query` and `workspace_act` record their inner
  capability in `details.capability`, now before any refusal. The query capabilities return it as
  a top-level `capability`, and the pages show it beside the action (`workspace_act create_agent`).
- **Polling.** `my_audit` leaves out successful reads unless `include_reads` is true: a read is a
  capability of kind `read` or one the policy checks as a read, such as `workspace_query`, and the
  set comes from the registry at call time. Failed and denied reads always appear.
  `audit::Filter::omit_successful_actions` carries the set to the store. `audit` keeps every event
  by default; it takes the same `include_reads` input.

`/activity` and `/audit` show the name (the recorded identity is the cell's title), the inner
capability, and an order selector; `/activity` also has an **Include reads** checkbox
(`?reads=all`). `tests/test_audit_isolation.cpp` (`ActivityIsNewestFirstNamedAndQuiet`, both
stores) and `tests/test_streams.cpp` (the acting agent of a task credential) cover it.

## Which log

Decision 52 (liquid-bridge `docs/decisions.md`, lake#317, lake#318) splits lake's audit by subject.
`audit_route` in `src/api/audit_routing.cpp` is the one function that decides, from the
capability and its input:

- **Central** (the hash-chained `audit_events` table): sign-in, sessions, tokens, users, customers,
  organizations, memberships, teams, invitations, grants and policy objects, workspaces themselves,
  SSO and SCIM, account recovery and MFA, workload identity, certificates and ACME, the keystore,
  lake's identity, transparency, TUF and timestamping, the audit capabilities, and the reads that
  span an account or the instance (`list_services`, `metering_usage`, `search`). Dynamic-secret
  leases stay central too: each event is appended to lake's transparency log, which commits to the
  central chain when the call is made.
- **The workspace's audit**: `workspace_act`, `workspace_query`, `workspace_sql` and every
  capability that acts on an object with a workspace: repositories and their sync, forge, drive,
  platform environments, applications, apps, builds, secrets, stores, services, streams, agents,
  experiments, Ask and Design. Pushes, branch protection decisions at a push and the deployment
  phases the platform sync mirrors are recorded outside the registry and go to the repository's
  workspace the same way.

Every registered capability is in exactly one of the two tables
(`AuditRouting.EveryRegisteredCapabilityIsClassified`); a new capability is refused by that test
until it is classified. A workspace capability's event goes to the workspace the call acted on:
the `workspace_id` `Registry::call` validated, else the workspace of the repository, secret mount,
application, app, build or agent task its input names (one indexed PostgreSQL read of a routing
row; no workspace is called). It stays central when:

- the call was refused before its handler ran (no credential, an invalid input, a missing,
  unknown or foreign `workspace_id`, no authority), or refused for want of authority (`denied`):
  authorization decisions are control-plane evidence, and a caller cannot write into a workspace
  it may not act in;
- no live workspace of the call's account can be found: an object without a workspace, a deleted
  workspace;
- this lake has no workspace audit: no workspace Zettas are configured, so the server has no
  `AuditRecorder` and every event is central (local development and the single-store tests).

A workspace event is never written centrally instead. Queue overflow writes a separate,
content-free delivery failure to the central chain, as described below.

## Workspace audit

A workspace's audit is the sealed record collection `lake_audit_events` of its own Zetta
([workspace records](../architecture/workspace-records.md)), found by one lookup
(`workspace:<id>`) and ordered by each event's instant. Zetta's own audit trail is not used: its
vocabulary is closed (`AuditAction`), its events are the engine's, not lake's signed `audit_event/1`
values, and its segments are checksummed, not hash-linked, so it would neither carry lake's
signature nor let lake's verifier check a chain. The record store is lake's, sealed with the
workspace's key, and conditional.

**Off the request path.** A call only queues its event in the server's `AuditRecorder`
(`include/lake/api/audit_recorder.hpp`), with its id and instant fixed when the call finished; no
workspace round trip is added to any call. The owner loop flushes the queue every second
(`ServerOptions::audit_flush_interval`), at once when half of `audit_max_pending` (65,536) is
held, and once more when the server drains. A flush signs each workspace's events in order and
appends them to its chain, one conditional `put_record` each.

**Capacity and delivery failure (LSR-098, lake#964).** The process holds at most
`audit_max_pending` events (minimum two), including events currently being flushed. One customer
can hold at most half that capacity, across all its workspaces; one workspace at most a quarter
(each allocation is at least one). At the default 65,536, these limits are 32,768 and 16,384.
An unavailable workspace cannot exhaust another customer's allocation by adding requests or
creating more workspaces. These are ceilings, not guaranteed reservations against many customers
exhausting the process together. Change the process limit through `ServerOptions::audit_max_pending`;
the customer and workspace limits follow it.

A rejected event returns `audit_queue_full` (HTTP 503). Before returning, Lake signs a separate
central `audit_delivery_failed` event containing only the customer, principal, request ID,
workspace ID, omitted event ID, original operation outcome and error code. It carries no workspace
payload, object name or audit details. If that central write fails too, the error explicitly says
that delivery evidence could not be recorded. REST and MCP preserve the completed operation's
original envelope in `operation_result`; a successful operation may already have changed state,
so inspect this result before retrying. Push and reference authorization use the same rejection
path and refuse publication when their audit cannot be accepted.

Accepted events keep their capacity while a flush runs, so arrivals cannot evict failed writes.
A workspace that is not ready keeps its accepted events in order for the next flush; deleted
workspaces release theirs and increment the deletion counter. Queued payloads remain in memory:
a durable payload retry queue is a follow-up. On a crash the loss window is the interval since the
last successful flush, plus any failing writes. The signed delivery failure records an overflow
gap; it does not reconstruct the omitted payload. Counters: `lake_audit_workspace_events_written_total`,
`lake_audit_workspace_flush_errors_total`, `lake_audit_workspace_conflicts_total`,
`lake_audit_workspace_dropped_total{reason="bound"|"workspace_deleted"}`, and the gauge
`lake_audit_workspace_pending`.

## Tamper evidence

Each workspace has its own **hash chain**, verified like the central one:

- Every event lake writes there is a signed `audit_event/1`, the central format, whose
  `previous_digest` is the digest of the workspace's previous chained event.
- The n-th event's record key is the blind index of `sequence:<n>`, written `if_absent`. Claiming a
  place in the chain is that one conditional write, so two lake replicas never fork it: the one
  that loses reads the place, searches on for the end of the chain (doubling probes, then a binary
  search) and signs its event again after it. A place that already holds the same event (a write
  whose answer was lost) counts as written.
- The record is sealed with the workspace's key and bound to its key and place, so a value moved
  from another place, collection or workspace does not open, and the workspace holds no plaintext.
- `verify_audit {workspace_id}` reads the whole audit and checks that the chained events form one
  unbroken chain from place 1, that every signature verifies against lake's identity history, and
  that every digest matches; a removed or altered event is `invalid_chain`, `invalid_signature` or
  `invalid_digest`, naming the place.

Events recorded centrally before the split and copied by `copy_workspace_records` keep their id,
signature and central predecessor; they are stored under their id, have no place, and are checked
alone (`audit::verify_event`). The central chain holds their place.

What it does not detect: removal of the newest events of a workspace (the end of a chain), or the
whole workspace rolled back by its administrator. Both need state outside the workspace, as for
every workspace record ([workspace records](../architecture/workspace-records.md#record-format));
a later change can anchor each workspace's chain head in lake's transparency log.

## Reading both logs

`audit`, `my_audit` and `export_audit` take two more inputs, and every returned event names its
log: `log` (`central` or `workspace`), `workspace_id`, `sequence` (its place, for a chained event)
and `pending: true` for an event this process has queued and not yet written (no signature yet).
The answer lists `unread_workspaces` (`workspace_id`, `code`, `message`) whose audit could not be
read; the rest is still answered.

- `workspace_id`: that workspace's audit alone. `after_id` cannot be combined with it; it pages the
  central log.
- `customer_id`: the account's audit: the central events attributed to it and the audits of its
  live workspaces.
- `log: central`: the central log alone (the Organization page's events).
- Otherwise, `audit` (operator only) reads the central log and every live workspace's audit;
  `my_audit` reads the central log and the audits of the workspaces of the caller's own accounts,
  and only the caller's own events from them. A workspace of another account is `unauthorized`.

The workspaces are read concurrently, one `list_records` walk each, all in one round, and merged
with the central events by instant, newest first unless `order` is `oldest`, at most `limit`, with
an event the copy put in a workspace before its central row is dropped counted once. The
`/activity` and `/audit` pages read through the same capabilities with `call_async`, show each
event's log (Central, or the workspace by name) and say which workspace could not be read; their
budget is one round, one `list_records` call per workspace
([performance](../ui/performance.md)). The repository's branch protection page reads its
workspace's audit. `audit`, `my_audit`, `export_audit` and `verify_audit` keep a synchronous form
for a lake without workspaces; with workspaces they are read through `Registry::call_async`.

## Moving the central events of workspaces (migration `0063`)

Migration `0063_audit_workspaces` records, in `workspace_assignments` (`object_table
'audit_events'`), the workspace of each central event about a workspace's data: an event of a
workspace action (the workspace capabilities, `push_change`, `reference_update`,
`deployment_phase`) that was not refused for want of authority, whose workspace is named in its
details (`workspace_id`, `repository_id`, `repository`) or found through its object (a repository,
app, build, secret mount or agent task), and belongs to the event's account. `event_workspace`: it
is copied there. `workspace_deleted`: its workspace is gone, and it stays central. Every other
event stays central, and the migration reports the count: most capability events recorded before
the split name only `capability/<name>`. Nothing is dropped, and `audit_events` stays append-only.

`copy_workspace_records {"collection":"audit_events"}` plans, copies (each event as it is, under
its id, `if_absent`) and verifies (the copy equals the central row and its signature verifies);
each run first reapplies the assignment with the current list, for events an older replica wrote
centrally during a rolling update. Nothing needs selecting, so `complete` is false only for a
missing or changed copy. A later, separately approved migration may replace each copied central
row by a digest-only row so the central chain still verifies.

### Cutover, dev cluster

These steps are for the coordinator and are not run by this change.

1. Workspace Zettas run Zetta `dev` at or after `82f617e` (the record store), as for every
   workspace record.
2. Have the operator vendor `db/schema.sql` through `0063_audit_workspaces` and select
   `audit_workspaces_guard`. Apply the schema, then read the warnings `audit events
   (event_workspace|workspace_deleted): N` and `audit events staying central: N`.
3. Deploy the lake image built from this change. From then on, events about a workspace's data go
   to its audit, a second after the call. `/activity` and `/audit` show both logs. The central
   chain keeps verifying; `verify_audit {"workspace_id": ...}` verifies each workspace's.
4. Run `POST /v1/copy_workspace_records {"collection":"audit_events","mode":"plan"}` with the
   operator bearer, then `{"mode":"copy"}` following `next_after` until it is null, then
   `{"mode":"verify"}` over every page. Until then the old events show once, from the central log.
5. Rerun step 4 after any rollback to an older image: events it wrote centrally are assigned and
   copied on the next run.
6. Nothing is dropped. Replacing the copied central rows by digest-only rows is a later decision.

## Verify queue isolation

`WorkspaceRecordStores.AuditQueueIsolatesUnavailableCustomersDuringFlush` fills two unavailable
workspaces of one customer, rejects more events from that customer, and writes another customer's
events while requests arrive during the flush. `AuditQueueOverflowPreservesResultsAndSignedDeliveryEvidence`
checks a committed transcript, an unavailable workspace failure, signed content-free evidence and
the direct push/reference recording path. Both run against memory and PostgreSQL. Run with
`ctest --test-dir build --timeout 600 -R 'AuditQueue|AuditEventsAboutAWorkspace' --output-on-failure`.
