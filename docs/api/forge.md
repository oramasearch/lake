# Forge records

**What.** A repository's forge, export, and GitHub-import facts. Each record belongs to a
repository, has a constrained kind, and retains a versioned JSON object as its value. Since
lake#341 every record lives in the workspace Zetta of its repository's workspace
([where they live](#where-forge-records-live)); migration `0012_forge_export_import` created the
retired PostgreSQL table `forge_records`, which the operator's one-shot copy moved and migration
`0070_drop_moved_tables` drops (lake#311).

**Why.** A review anchor is an evolving value: it initially holds `{change, path, line, column}`.
When an author rebases a reviewed change as a rewritten candidate, Liquid maps that cursor through
the repository diff and Lake stores the mapped position and disposition without reinterpreting the
original comment.

## Stored kinds

`issue`, `issue_comment`, `review`, `review_comment`, `release`, `webhook`, `webhook_delivery`,
`branch_protection`, `export`, and `github_import` are the allowed values
(`control::forge_record_kinds`). A record of any other kind, or with a value that is not an object,
is refused before it is written.

Each record has a UUID and creation/update timestamps (whole seconds, `YYYY-MM-DDTHH:MM:SSZ`).
Values are intentionally immutable facts where possible; state transitions retain the same record
identifier and replace only its versioned value. Secrets such as webhook signing material are never exposed in a list or export result.

Every forge `list_*` capability is a repository candidate read. It resolves the requested
repository and authorizes its `read` action before returning records. Forge lists therefore remain
authenticated and never turn the public-repository exception into anonymous issue, review, release,
delivery, or policy access.

## Where forge records live

Every forge record is a sealed record of the `lake_forge_records` collection in the workspace
Zetta of its repository's workspace (`repositories.workspace_id`, lake#315), through the
[workspace records](../architecture/workspace-records.md) mechanism: one record per forge record,
keyed by its id, found by the lookup `repository:<id>` + `kind:<kind>`. Listing one kind of one
repository is one `list_records`.

- **The workspace is the repository's.** No forge capability takes a `workspace_id`: the
  repository a caller names has one immutable workspace, so a caller who names the repository has
  named the workspace, and every record of one repository is in one place. Nothing falls back to a
  default workspace, the lake Zetta or PostgreSQL.
- **Ids.** A missing `repository_id` is `invalid_input` at `/repository_id`; an unknown one is
  refused at `/repository_id`. A record id of another repository, of another kind, or not a UUID
  is `not_found` at the field that named it (`/issue_id`, `/review_id`, `/webhook_id`,
  `/delivery_id`).
- **A repository without a workspace** (one migration 0051 recorded as `needs_selection`) has
  nowhere to keep forge records: every forge call on it is `conflict` at `/repository_id` until an
  operator assigns its workspace.
- **A workspace that is not ready** refuses with `workspace_not_ready`; a deleted one with the
  workspace service's refusal. Deleting a workspace deletes its repositories with it (lake#514,
  [workspaces.md](workspaces.md#deletion)), and their forge records go with its Zetta.
- **Immutable ownership.** The sealed value is bound to its workspace, collection, key and lookup,
  so a record copied into another workspace or repository does not open there.
- **Updates** (`update_issue`, `update_review`, `set_branch_protection` on an existing rule,
  `map_review_anchors`) read the record and write it only at the engine revision they read,
  rereading and applying the change again on a conflict. Two updates of different fields both
  survive.
- **Callers.** REST, MCP and the repository pages reach the records only through these
  capabilities; `export_repository`, `import_repository` and `import_github_repository` read and
  write them in the repository's workspace; the push path reads branch protection there
  ([below](#branch-protection)).

## Issues and reviews

`create_issue` creates an open issue with Markdown `body`, labels, assignees, and explicit
references. A reference is an object such as `{ "change": "..." }` or `{ "path": "..." }`.
`update_issue` changes its fields or its state (`open` or `closed`), and `create_issue_comment`
adds Markdown discussion after confirming that the issue belongs to the repository.

**Author (lake#598).** `create_issue` and `create_issue_comment` record the caller's TAAP identity
as the record's `author`. Lake sets it; the input cannot name one (the schemas refuse an `author`
member), and `update_issue` keeps it. Records written before lake#598 have no `author`, and a GitHub
import keeps the GitHub login it read. Review comments are unchanged: liquid's forge client reads
a review comment's `author` as an actor object. A review records its opener as `opened_by`
([approvals](#who-may-approve-lsr-077)).

**People to assign (lake#598).** `issue_assignees` (`repository_id`) answers
`{"people": [{taap_id, kind, display_name, email, handle}]}`: the person who owns the repository's
workspace, or for an organization's workspace the organization and each person who is a member of
it or of one of its teams, the same people `drive_share_people` lists for a folder
(`src/api/repository_people.cpp`). `assignees` holds their `taap_id`s. It needs `write` on the
repository, as assigning does, so a reader cannot list a workspace's people and their emails;
lake does not check that an assignee is one of them. Its audit goes to the repository's workspace.

Labels are free text on each issue; lake keeps no label list or colours. The web app derives a
label's colour from its name (`labelTone`, `frontend/src/pages/repositories/issues.ts`).

`create_review` binds an open review to exactly one liquid `change_id`. `update_review` transitions
that review to `approved` or `closed`. `create_review_comment` verifies that its parent review is
in the same repository and accepts an anchor value with required `change`, `path`, `line`, and
`column` members.

### Who may approve (LSR-077)

A protected branch's review requirement needs two people, the one who pushes and one who approves.
Before LSR-077 any repository writer could open a review of their own change, approve it and push
it, and the branch accepted the approval.

- `create_review` records the caller as `opened_by`; the input cannot name it.
- `update_review` with `approved` needs `review` on the repository as well as the `write` every
  review update needs (the `admin` role, directly or inherited, holds both; `write` alone is
  `unauthorized`) and is `forbidden` to the review's opener, the operator included. Lake writes
  `approval: {by, change_id}`, the caller and the review's change; the input cannot name either.
  Any other state sets `approval` to `null`, withdrawing it.
- `import_repository` drops `approval` from every review it imports, so an archive cannot carry an
  approval in; an imported review keeps its state as history.

`map_review_anchors` receives a source change, target change, path, and the text hunks between them.
It updates only matching comment anchors and reports `same`, `shifted`, `approximate`, or
`deleted`. `liquid::forge_client::advance_anchors` loads the review comments, derives those hunks
from Liquid's repository diff, persists each mapped cursor through this capability, and retargets
the review to the rewritten change.

Named review references are synchronized with the repository document. Lake's repository history
adapter walks the current head and every named reference, deduplicating change identities, so
`get_change`, `change_diff`, and the change page can resolve an unlanded reviewed change.

## Graph-backed change checks

The change page reads build results through `list_artifacts`. Each listed result carries the same
output, verified attestation, and retained trace evidence as `get_artifact`; the adapter does not
invent a page-only check state. The renderer names every output path and every verified
attestation profile beside the action and result identity.

The end-to-end scenario records a graph-selected action and signed `slsa_provenance/1` statement
without adding automation YAML to the repository. Its final HTML assertions require the output
path, profile, and result identity that Liquid persisted.

## Releases

`create_release` binds a tag and change identity to selected build-result IDs. Each result must
have a verified attestation. A result with verified source provenance for a different repository
or change is refused with `conflict` at `/artifact_ids`, before publication or record creation.
Unbound legacy evidence remains readable and can accompany an ordinary release; its artifact
snapshot says `source_binding: "source not bound"`. Bound snapshots say `"verified"`. The snapshot
also retains the result ID, action, output count and verified attestation identifiers and subjects.

### Source binding (LSR-106)

A source binding comes only from `build::verify_provenance` succeeding for the selected result:
it checks the result signature, SLSA signature, result subject and action/predicate relationship.
Lake then reads `LAKE_SOURCE_REPOSITORY_ID` and `LAKE_SOURCE_CHANGE_ID` from that signed SLSA
`buildDefinition.externalParameters.environment`. Both must be nonempty and match Lake's exact
repository UUID and the release's exact change ID. No aliases, case folding or input-tree equality
substitute for a change ID. Generic signed attestations and SBOMs do not establish a source binding.
The pinned libliquid format has no dedicated source-revision field; these two signed environment
keys provide the explicit claim without changing its record format.

For `required_verification`, Lake reopens every selected result from this repository before each
push and checks the binding again. Every artifact in a qualifying release must match; a missing,
unbound or malformed result cannot count. A repository-store error refuses reference updates.
Stored release snapshots, including portable imports and pre-upgrade releases, cannot supply the
claims used by branch protection. Evidence and release reads remain outside the synchronous push
callback; the callback accepts only the change IDs freshly validated during that push's load.

Producers must put the two keys in the **libliquid action environment before the action/result
and SLSA records are signed**. Setting environment variables only on the Lake server, adding
unsigned release fields, or signing an unrelated statement is insufficient. Lake's release path
copies evidence; it does not produce libliquid build attestations. Operator-managed container
builds need a producer update to supply these signed claims before their results can satisfy
protected-branch verification. No operator code or dependency pin changes here.

Verify with `Forge.ReleaseRejectsArtifactsBoundToAnotherSource`,
`Forge.RequiredVerificationRechecksEveryReleaseArtifactSource` and
`RepositoryHttp.PublicRepositoriesAreReadableAndPrivateRepositoriesAreHidden`. The HTTP fixture
creates real signed libliquid provenance and proves a matching release passes while a different
change is refused. Changes to this contract belong in `RepositoryArtifact::verified_source`, the
result-bound verifier adapter in `http_server.cpp` and the release/push checks in
`forge_capabilities.cpp`; keep these tests and this page together.

## Webhooks

`create_webhook` stores an event subscription, endpoint, and signing secret. Lake sends events for
issues, reviews, and releases only to subscriptions that name that event or `*`. Each attempt has
canonical JSON payload bytes, an `x-lake-event` header, and an HMAC-SHA-256
`x-lake-signature-256` header. The durable delivery record retains the status, failure text,
signature, payload, and attempt count. `retry_webhook_delivery` creates a new attempt only when a
prior attempt was unsuccessful; it never overwrites the event log.

**How a delivery is sent (lake#772, lake#876).** The endpoint belongs to the customer, so Lake
assumes it may be slow, hostile or pointed somewhere it should not be:

- **Off the server's thread.** The request runs on the blocking pool (`run_blocking`), never on the
  thread that serves every other request. Before lake#772 a delivery to an endpoint that did not answer
  stopped Lake answering anything, `/healthz` included, until the liveness probe restarted the pod.
- **10 seconds in all.** The name lookup, the connection, the TLS handshake and the exchange are each
  bounded, so a delivery gives up within about 10 s and the attempt is recorded with its error.
- **HTTPS to public addresses only.** Every address the host resolves to is checked at connect time,
  as for MCP connections (`mcp_public_address`): loopback, private, link-local, CGNAT and metadata
  addresses are refused with "the outbound host's address is not a public address", and plain HTTP
  with "outbound URL must use HTTPS". Before lake#876 a webhook could make Lake POST to its own
  network. `LAKE_WEBHOOK_ALLOW_PRIVATE_ADDRESSES=1` lifts the address check for local development
  and tests only; never set it on a hosted Lake.

The same rules apply to [application lifecycle webhooks](apps.md#lifecycle-webhooks). Verify with
`WebhookDelivery.ASilentEndpointDoesNotStopLakeAndTimesOut` and
`WebhookDelivery.PrivateAddressesAndPlainHttpAreRefused` (`tests/test_http_server.cpp`).

The secret is never answered after `create_webhook`: `list_webhooks` and `export_repository` leave
it out, and a webhook imported from an archive without one signs with a new random secret
([export](export.md#what-an-archive-never-carries), LSR-024).

**Who reads a webhook (lake#1314).** A receiver URL is often a credential itself: a chat
incoming-webhook URL, or a CI trigger with a token in its path. `list_webhooks` and
`list_webhook_deliveries` therefore need `write` on the repository, the same as `create_webhook`,
`deliver_webhook` and `retry_webhook_delivery`; a reader is `unauthorized`, and neither is open to
anonymous readers of a public repository. `export_repository` leaves webhooks and their deliveries
out of the archive of a caller without `write`. Verify with
`Forge.OnlyWritersReadWebhooksAndTheirDeliveries` and
`Export.AnArchiveCarriesNoWebhookSecretAndGrantsOnlyForAnAdministrator` (`tests/test_api.cpp`).

The sender is an injected service at the API boundary so the registry and its tests do not perform
network I/O. The HTTP service supplies that sender in deployment. If a sender is unavailable, the
attempt is retained as a failed delivery and can be retried when delivery is configured.

## Branch protection

`set_branch_protection` creates or updates one policy rule for an exact ref or a trailing-`*`
prefix. A rule may require an approved review for the proposed change, verified release evidence,
fast-forward-only updates, and a non-empty allow-list of TAAP identities. The rule also owns a
repository-child `path` policy object. A non-operator pusher must be permitted to `write` that
object through `authorize`; inherited repository grants therefore remain effective and ref-specific
grants can narrow access.

Liquid #192 supplies `BranchRefUpdate` values containing the ref name, server-derived prior and
proposed heads, and computed fast-forward result. Lake evaluates every reference callback with the
authenticated caller and returns one accepted or refused verdict per reference. A refusal names the
reference and carries the rule's reason verbatim; for example,
`branch protection rejects a non-fast-forward update to main`. The registry refuses an anonymous
or unauthorized push before invoking the reference evaluator.

The rules, and the reviews or releases a rule asks for, are read from the repository's workspace
once, before the push starts (`load_branch_protection`): the push itself runs synchronously inside
the repository store, so it evaluates that snapshot rather than call the workspace from inside it.
Reviews are read only when a rule requires a review, releases only when a rule requires
verification, so a repository without such rules costs one `list_records` per push. When the
rules cannot be read (the repository has no workspace, or its workspace is not ready or not
reachable), every reference update of the push is refused with that reason, recorded as a
`forge.branch_protection.refused` event with rule `branch_protection_unavailable`; an atomic push
then changes no reference, and an ordinary one applies none of them. A lake configured without any workspace service holds no forge records anywhere, so it
evaluates no rules.

An approved review is a `review` record whose `change_id` exactly equals the reference's proposed
head, whose current `state` is `approved`, and whose `approval` lake recorded for that same change
([who may approve](#who-may-approve-lsr-077)). The approver must not be the pusher or the review's
opener, and must hold `review` on the rule's `path` object (which inherits the repository's grants)
when the push is evaluated, so a revoked reviewer's approval stops counting. Approval for an
ancestor, a different change, a review in any other state, or a review approved before LSR-077
(which has no `approval`; approve it again) does not satisfy `required_review`. The operator
credential is one principal: its approval counts for another person's push, never for its own.

Lake records one signed audit event for each protected reference decision. Allowed decisions use
kind `forge.branch_protection.allowed`; refusals use `forge.branch_protection.refused`. Both retain
the repository, reference, caller, matched rule, and reason, never a credential.

An ordinary push applies accepted references independently and leaves refused references at their
pre-push heads. The successful reply reports every requested reference as `applied` or `refused`;
refused entries include the exact rule reason. A client that requires all-or-nothing publication
sets `atomic: true`. Any refusal then leaves every reference unchanged, preserving the original
refusal reason and reporting otherwise accepted entries as refused because the atomic push failed.

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The forge capability tests create and read records in a test workspace Zetta.
`Stores/WorkspaceRecordStores.ForgeRecordsLiveInTheirRepositorysWorkspace` covers two accounts with
two workspaces each, records stored only at their own endpoint, ids of other repositories and
workspaces, conditional updates, least privilege, a repository without a workspace, a workspace
that is not ready and workspace deletion, for reads and for branch protection.
`...TheOneShotCopyMovesForgeRecordsIntoTheirRepositorysWorkspace` and
`PostgresForgeRecords.MigrationRecordsEachRowsOwnerForTheCopy` cover the copy and migration 0054.
The `PostgresForgeScenario` case exercises the Liquid forge client, cursor movement, approval by a
second person, publication, and graph evidence through a real Lake server with a workspace Zetta.
`Forge.ARequiredReviewNeedsASecondPersonWithReview` covers who may approve: the opener's own
approval, a writer without `review`, the pusher's own approval, a second person's, a withdrawn one
and a revoked reviewer's; `Export.VerifiesAndRecreatesPortableRepositoryFacts` covers an imported
review losing its approval.

## Change

Add a forge kind to `control::forge_record_kinds` and the capabilities that use it. A body change
that is not additive raises `ForgeRecordCodec`'s schema version
([schema evolution](../architecture/workspace-records.md#schema-evolution)).

## Migration and cutover (lake#341)

Migration `0054_forge_record_workspaces` records each `forge_records` row's owner in
`workspace_assignments`: `repository_workspace` (its repository's workspace), `needs_selection`
(the repository has no workspace yet; resolved by the copy once it has one) or `workspace_deleted`
(the workspace is gone, and the row with it; it does not hold the copy back). It adds no column and
drops nothing. The dev cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-forge_records-dev-cluster).
