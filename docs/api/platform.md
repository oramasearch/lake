# Platform environments

Lake is a read/write client of the operator's Zetta control plane, not a deployment control
plane. `liquid deploy` appends deployment intent, the operator appends deployment status and
reconciles environment/domain requests, and Lake mirrors those rows for authorized pages and
audit. Lake never appends a deployment row; rollback is another `liquid deploy --to` operation.

## Where environments live (lake#358, lake#359)

A repository's environments and the rows Lake mirrors for it live in the workspace Zetta of the
repository's workspace (`repositories.workspace_id`, lake#315), as two sealed collections of
[workspace records](../architecture/workspace-records.md), both found by the repository:

| Collection | One record per | Key | Written |
|---|---|---|---|
| `lake_platform_environments` | environment | its id, `md5(repository id, newline, name)` as a UUID | `if_absent` when the sync first sees it; two replicas create it once |
| `lake_platform_mirror` | deployment, environment value or domain | `md5(kind, newline, key)` as a UUID | `if_absent`, or at the engine revision read when the stored row is older (the row order below); an older row never replaces a newer one |

The caller never chooses the workspace: it is the repository's, so naming the repository names
it, and nothing falls back to another workspace, the lake Zetta or PostgreSQL. A repository
without a workspace is `conflict` at `/repository_id`; a workspace that is not ready is
`workspace_not_ready`. Deleting a workspace deletes its Zetta and these records with it.

Each environment's `environment` policy object stays in the control store with its grants, under
the environment's id as its external id. Authorization therefore never reads the workspace:
`get_environment`, the logs and the writes find the object from the repository and the name, and
`list_environments` and `list_deployments` answer an empty list, without a workspace call, to a
caller who may read none of the repository's environment objects. PostgreSQL keeps no
environment name, value, domain or deployment row; the object's external id is an md5 of the
repository and the name.

Reads cost one `list_records` per collection: the environments page two, an environment page
three (the environment record, then the rows for the document and for the history). The
Deployments overview reads only the repositories with an environment the caller may read. Setting
an environment value reads the environment's mirror rows once more, to find the key the operator
uses for it ([below](#which-repository-a-row-names)).

## Capabilities

All capabilities resolve the repository's local `environment` policy object and go through
`Registry::call`. Environment objects have `read`, `write`, `deploy`, `share`, and `admin` roles.

| Capability | Action | Input | Value |
|---|---|---|---|
| `list_environments` | `read` per candidate | `repository_id` | environment summaries with id, current phase, change, image, URL, domains, and update time |
| `get_environment` | `read` | `repository_id`, `environment` | current deployment, deployment history, domains, plain environment values, and optional `stale_since` |
| `list_deployments` | `read` per candidate | `repository_id`, optional `environment`, optional `limit` | latest deployment record per deployment id, newest first |
| `environment_logs` | `read` | `repository_id`, `environment`, optional `since`, `limit`, `direction` | `{items: [{ts,pod,container,line}], next_since}` |
| `set_environment_env` | `write` | `repository_id`, `environment`, `key`, `value` | appended row identity |
| `unset_environment_env` | `write` | `repository_id`, `environment`, `key` | appended tombstone identity |
| `create_domain` | `deploy` | `repository_id`, `environment`, `host`, `tls` | the host's claim and the TXT record that proves control (`verification`); `tls` is `acme` or `provided`. A host that is one of this Lake's own names is `invalid_input` at `/host` (lake#949): equal to or below its public host, the zone that host is in, an alias (`LAKE_HOST_ALIASES`) or the apps wildcard. Nothing reaches the operator until `verify_domain` ([custom domains](#custom-domains)) |
| `verify_domain` | `deploy` | `repository_id`, `environment`, `host` | reads `_lake-challenge.<host>`; when it holds the claim's token, the claim is `verified` and the pending domain request is written for the operator |
| `delete_domain` | `deploy` | `repository_id`, `environment`, `host` | releases the environment's claim and writes the pending domain tombstone |

Environment values are plain configuration only. Before appending, Lake runs libliquid's inline
secret detector over `KEY=value`; a match is refused with `use a sealed secret`. Sealed-secret
references are tracked separately in #129.

## Custom domains

A custom domain is claimed before anything routes it (lake#949). Claims are in
lake's Postgres control plane, `domain_claims` (migration `0106_domain_claims`,
[schema](../control-plane/schema.md#domain-claims-migration-0106_domain_claims)): one row per host,
across every account of this Lake, so a host belongs to one environment at a time. It is
multitenancy (which environment of which account holds a name), not workspace data.

1. **`create_domain` claims the host** in one statement. The host is lowercased (a trailing dot
   dropped). Another environment holding it, of any account or of the same repository, is
   `409 conflict` at `/host`. The same environment asking again answers the same claim and token
   (and takes the new `tls`). The answer:

   ```json
   {"repository_id": "…", "environment": "production", "host": "www.example.com", "tls": "acme",
    "status": "pending", "deleted": false,
    "verification": {"state": "pending", "verified_at": "",
                     "record": {"type": "TXT", "name": "_lake-challenge.www.example.com",
                                "value": "<64 lowercase hex digits>"}}}
   ```

   No domain row is written yet: the operator never sees an unverified claim.
2. **The tenant publishes the TXT record** at their DNS provider: name `_lake-challenge.<host>`,
   value the token. Other TXT records at that name are ignored.
3. **`verify_domain` reads it.** Lake asks the name servers of its own host (`/etc/resolv.conf`; in a
   pod, the cluster's DNS) over UDP, again over TCP when the answer is truncated, two seconds per
   server (`src/control/dns_txt.cpp`). When one record equals the token, the claim becomes
   `verified` (`verified_at` set) and lake appends the domain row to `orama_control.domains`, as
   `create_domain` used to. Otherwise it is `409 conflict` at `/host` (no record yet, or not this
   claim's token) or `503 store_unavailable` (no name server answered); the claim stays `pending`
   and can be verified again. A host this environment has not claimed is `404 not_found`. Calling it
   on a verified claim writes the request again, which retries a row write that failed.
4. **The operator acts only on verified claims.** It reads `domain_claims` from the control plane
   and refuses, with the reason on the domain's status row, a domain row whose host has no verified
   claim of that repository and environment (orama-operator, `planLakeDomains`).
5. **`delete_domain` releases the claim** (only this environment's) and writes the tombstone. The
   host is then free to claim again.

A verified claim is not checked again: removing the TXT record later does not unroute the host.
Pending claims do not expire. The web app shows the record after **Create domain** and verifies it
with **Verify**; `create_domain` again shows the same record. Tests: `Stores/DomainClaims.*`
(`tests/test_domain_claims.cpp`, both stores), `DnsTxt.*` (the TXT question and answer), and
`PlatformFixture.AnUnverifiedDomainIsNeverWrittenForTheOperator`,
`VerifyDomainFailsUntilTheRecordHoldsTheToken` and `AHostIsClaimedByOneEnvironmentAcrossCustomers`
(`tests/test_platform.cpp`).

## Proposed operator rows

These schemas are the Lake-side proposal pending liquid #84 and operator #124. Column names have
one code source in `include/lake/control/zetta_control.hpp`.

| Table | Columns | Writer |
|---|---|---|
| `orama_control.deployments` | `operator, lake, repository, environment, kind, change, image, deployment_id, requested_by, requested_at, phase, namespace, url, message, updated_at, ordering_token` | desired rows by liquid; status rows by the operator. `repository` is lake's id or liquid's ([which repository a row names](#which-repository-a-row-names)) |
| `orama_control.environment_env` | `lake, repository, environment, key, value, deleted, set_by, updated_at, ordering_token` | Lake; operator reads |
| `orama_control.domains` | `lake, repository, environment, host, tls, status, message, deleted, requested_by, updated_at, ordering_token` | requests by Lake; operator materializes status |

Rows are append-only. Lake writes `updated_at` as a UTC ISO 8601 timestamp with exactly six
fractional digits and a UUID `ordering_token` on every environment-value and domain row. Reads
select rows for `lake = LAKE_PUBLIC_HOST` and choose the lexicographically greatest
`(updated_at, ordering_token, canonical row)` tuple per `deployment_id`,
`(repository, environment, key)`, or `(repository, environment, host)`. This total order means
every permutation of a set of rows produces the same current value and deployment-phase sequence.
Consumers may rely on a later set or tombstone winning when its tuple is greater; an immediate set
followed by an unset therefore resolves to the tombstone, and the reverse resolves to the set.

Rows written before this contract have no `ordering_token`; Lake treats the missing field as the
empty string and expands their second-resolution timestamps to zero microseconds for comparison.
The canonical row is the final tie-break for legacy rows, so historical and mixed rows also reduce
deterministically. There is no Lake Postgres migration: the token remains in the existing mirrored
JSON document, and older Zetta rows project the absent field as null.

`deleted = true` is the environment/domain tombstone. The Zetta SQL capability is GET-bound and
accepts one statement, so Lake emits only fixed-table `SELECT` and `INSERT` statements. Values are
SQL literals quoted by Lake: apostrophes double, values must be scalar, and ASCII control
characters are rejected. The operator bearer is a Zetta `secret_header` and is never logged.

## Mirror and failure behavior

Every `LAKE_PLATFORM_SYNC_SECONDS` (default 15), the owner loop fetches all three tables, selects
the latest rows and groups them by repository. For each repository it opens the repository's
workspace, reads its environments and mirror rows (one `list_records` each), creates an
environment and its policy object on first sight, and writes each row the workspace does not
hold yet. Each newly observed deployment phase records one signed `platform.deployment.phase`
audit event before its row is written, so a sync that fails there records it next time rather than
never. A replica remembers what it last wrote per repository and skips a repository whose rows
have not changed, so a quiet sync makes no workspace call; every 20th sync rereads every
repository's workspace, which rewrites a workspace that lost its records.

All three reads must succeed before a sync writes anything. If the operator control plane is
unreachable, existing reads continue from the workspace mirror and include `stale_since`; health
reports `platform: degraded`, `/readyz` remains governed by the local control store, and platform
writes fail `store_unavailable`. A successful sync clears the stale marker. A repository whose
workspace cannot be written (not ready, or the repository has no workspace) is marked stale on its
own: its reads carry `stale_since` until a later sync writes it, and the service stays healthy. A
row whose `repository` names no single lake repository belongs to no workspace: it is not mirrored,
and it is reported ([below](#which-repository-a-row-names)), never skipped quietly.

## Which repository a row names

`liquid deploy` runs in a checkout and sends, as the row's `repository`, the id liquid gave the
repository (`repository::id()`, 16 bytes as 32 hex digits; liquid `src/deploy.cpp`), not lake's
UUID. A checkout made with `liquid clone` takes the identity of the lake repository it was cloned
from, and a repository lake created empty takes the identity of the first push it receives, so a
lake repository and its checkouts share one liquid id (lake#805). The operator copies the value into
its rows unchanged, and lake places each row with it:

1. A UUID is lake's own repository id (`get_repository`).
2. 32 hex digits, in either case, are a liquid id: first the UUID with the same 16 bytes, should
   liquid ever be given lake's id, then `repositories.liquid_id` (migration
   `0088_repository_liquid_ids`, indexed). Exactly one live repository must have it.

Lake records `repositories.liquid_id` after every push (a push never fails because of it; a read
error is put in the push's audit details) and, for repositories it has not read yet, during the
sync: when a row names a liquid id no repository has, the sync reads the liquid id of every live
repository that has a workspace and none recorded (opening its store), then places the row again.
It does so at most once every `full_sync_every` polls (20, about five minutes) while such a row
remains, so an unknown id does not reopen repositories on every poll.

A liquid id belongs to the account whose repository recorded it first (lake#1313). Anyone who can
read a repository (a public one, or one shared with them) can clone it and push the clone into an
empty repository of their own account, and the copy then has the original's liquid id. Lake does
not record a liquid id on a repository while a live repository of another account holds it:
`Store::set_repository_liquid_id` answers `conflict` ("another account's repository holds this
liquid id", naming no repository), the push still succeeds and carries the refusal in its audit
details under `liquid_id`, and the sync leaves the copy unrecorded. The original records its id on
its first push, before any copy can exist, so another account's copy never decides whether the
original's rows are placed. Repositories of one account that share an id are recorded and reported
as shared (below). A deleted holder keeps nothing, and clearing an id is never refused. Both stores
apply the rule; PostgreSQL takes a transaction advisory lock on the id before it checks, so two
accounts recording the same id at once take turns.

The workspace keeps the row with `repository` set to lake's id, so the pages and the record
checks find it, and with the operator's value in `operator_repository` when it was another one. The
operator matches environment values and domains to a deployment by its exact `repository` value, so
`set_environment_env`, `unset_environment_env`, `verify_domain`, `create_domain` and `delete_domain` write their rows
under the `operator_repository` of the environment's current deployment, or lake's id when it has
none.

A value that names no single repository is reported, with the reason:

| Reason | When |
|---|---|
| `no repository has this id` | a UUID that is not a lake repository |
| `no repository has this liquid id` | a liquid id no live repository has, after lake has read the ids it lacked |
| `the liquid id of N repositories (…); deploy from a checkout of one of them` | two repositories of one account share the liquid id (one was pushed from a clone of the other); lake does not guess |
| `not a lake or liquid repository id` | anything else |

The report is the poll's answer `unresolved_repository` (or `workspace_not_ready`, which then names
both), naming each value and reason; `PlatformService::unresolved_repositories()` with the reason
and since when; `platform_unresolved_repositories` (a count) on `GET /healthz`; the gauge
`lake_platform_unresolved_repositories`; and one `platform_sync_incomplete` log line on standard
error each time the poll's problem changes. The operator control plane answered, so `platform`
stays `ready`; the other repositories' rows are mirrored as usual.

Verify with `GET /healthz` (no `platform_unresolved_repositories`) and with `list_deployments` on
the repository a checkout was cloned from after `liquid deploy`. `LiquidIdFixture.*` in
`tests/test_platform.cpp` covers the three cases (liquid's id shown, lake's id shown, an unknown or
shared id reported) and `LiquidIdFixture.AnotherAccountsCopyDoesNotStopTheOriginalsRows` another
account's copy, `*.RecordsAndFindsLiquidIds` the store on both stores (another account's copy
refused included), and `SyncHttp.PushRecordsTheRepositorysLiquidId` (`tests/test_api.cpp`) the push,
an empty repository of another account pushed from a clone included.

To change how a value is placed, change `PlatformService::sync` (`src/control/platform.cpp`) and this
section together.

The retired PostgreSQL tables `platform_environments` and `platform_mirror` (migration
`0019_platform`) were read only by the one-shot copy; migration `0060_platform_workspaces` records
each row's owner and gives each environment's policy object its derived id. The copy was verified
on dev, and migration `0070_drop_moved_tables` drops both tables (lake#311); the environments'
policy objects stay. The cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-platform-environments-and-mirror-dev-cluster).

## A lake without the platform mirror

A lake started without `LAKE_OPERATOR_ZETTA_URL` and `LAKE_OPERATOR_ZETTA_TOKEN` (and without
`LAKE_LOGS_URL`) has no platform service. Every platform capability (`list_environments`,
`get_environment`, `list_deployments`, `set_environment_env`, `unset_environment_env`,
`create_domain`, `verify_domain`, `delete_domain`, `environment_logs`) then answers `501 not_supported`:

```json
{"ok": false, "error": {"code": "not_supported", "path": "",
 "message": "deployments are not set up on this lake: an operator turns them on by connecting lake to the cluster operator's platform source (docs/api/platform.md#a-lake-without-the-platform-mirror)"}}
```

It was `503 store_unavailable` before lake#575, which reads as an outage a client should retry;
asking again changes nothing until an operator sets those variables, so the code says the feature
is missing, and the web app's Ship → Deployments shows "Deployments Are Not Set Up on This Lake"
once, with no retry ([web app](../ui/web-app.md#how-a-page-runs)). The message names no engine,
because the web app shows it to people. `store_unavailable` stays the answer when the mirror is
configured and the operator control plane is unreachable (above).

To turn Deployments on, set both variables in the `Lake` CR's `spec.env` (the operator's Zetta
URL and a bearer it accepts) and restart lake. Verify with
`GET /v1/list_environments?repository_id=<id>`, which then answers `200` with the repository's
environments (`items`), and with `GET /healthz`, which reports `platform: ready`.
`PlatformFixture.WithoutThePlatformMirrorEveryCallSaysDeploymentsAreNotSetUp`
(`tests/test_platform.cpp`) covers the answer.

`server_info` says it too, without authentication: `deployments` is `true` when the platform
service is configured and `false` otherwise (lake#605). Ship → Deployments reads it first, so on a
lake without the mirror its one call is `server_info`: it neither lists repositories nor asks any of
them for environments. A lake from before lake#605 does not send the field; the page then asks the
first repository alone and the others only once it answered something other than `not_supported`
(`frontend/src/pages/apps/deployments.ts`, `frontend/tests/deployments.test.ts`).
`PlatformFixture.ServerInfoSaysWhetherDeploymentsAreSetUp` covers the flag. Verify with
`curl -s <lake>/v1/server_info`, and in the browser's network panel: Ship → Deployments on a lake
without the mirror sends `server_info` and nothing else.

## Loki-compatible logs

`environment_logs` takes the current deployment namespace and sends:

```text
GET <LAKE_LOGS_URL>/loki/api/v1/query_range
  ?query={namespace="<namespace>"}&start=<since>&limit=<limit>&direction=<direction>
```

`start` is omitted when `since` is absent. Direction is `forward` or `backward`; limits are
clamped to 1–1000. `LAKE_LOGS_TOKEN`, when configured, is a secret bearer header. Lake converts
Loki matrix streams to timestamped pod/container lines and returns the final timestamp as
`next_since`. There is no websocket log API.
