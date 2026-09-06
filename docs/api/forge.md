# Forge records

**What.** Migration `0012_forge_export_import` persists forge, export, and GitHub-import facts in
`forge_records`. Each row belongs to a repository, has a constrained kind, and retains a versioned
JSON object as its value. The API capabilities that create and read these facts are added with the
forge surface.

**Why.** A review anchor is an evolving value: it initially holds `{change, path, line, column}`.
When an author rebases a reviewed change as a rewritten candidate, Liquid maps that cursor through
the repository diff and Lake stores the mapped position and disposition without reinterpreting the
original comment.

## Stored kinds

`issue`, `issue_comment`, `review`, `review_comment`, `release`, `webhook`, `webhook_delivery`,
`branch_protection`, `export`, and `github_import` are the allowed values. The repository foreign
key and the kind constraint are enforced by Postgres; both store implementations require an object
value and a known repository.

Each record has a UUID and creation/update timestamps. Values are intentionally immutable facts
where possible; state transitions retain the same record identifier and replace only its versioned
value. Secrets such as webhook signing material are never exposed in a list or export result.

Every forge `list_*` capability is a repository candidate read. It resolves the requested
repository and authorizes its `read` action before returning records. Forge lists therefore remain
authenticated and never turn the public-repository exception into anonymous issue, review, release,
delivery, or policy access.

## Issues and reviews

`create_issue` creates an open issue with Markdown `body`, labels, assignees, and explicit
references. A reference is an object such as `{ "change": "..." }` or `{ "path": "..." }`.
`update_issue` changes its fields or its state (`open` or `closed`), and `create_issue_comment`
adds Markdown discussion after confirming that the issue belongs to the repository.

`create_review` binds an open review to exactly one liquid `change_id`. `update_review` transitions
that review to `approved` or `closed`. `create_review_comment` verifies that its parent review is
in the same repository and accepts an anchor value with required `change`, `path`, `line`, and
`column` members.

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

`create_release` binds a tag and change identity to selected build-result IDs. Lake reads every
result from the repository store and refuses a release unless each result has at least one verified
attestation. The release record copies each selected result's ID, action, output count, and verified
attestation identifiers and subjects. This creates a durable release-to-evidence link even when
build records are later queried independently.

## Webhooks

`create_webhook` stores an event subscription, endpoint, and signing secret. Lake sends events for
issues, reviews, and releases only to subscriptions that name that event or `*`. Each attempt has
canonical JSON payload bytes, an `x-lake-event` header, and an HMAC-SHA-256
`x-lake-signature-256` header. The durable delivery record retains the status, failure text,
signature, payload, and attempt count. `retry_webhook_delivery` creates a new attempt only when a
prior attempt was unsuccessful; it never overwrites the event log.

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

An approved review is a `review` record whose `change_id` exactly equals the reference's proposed
head and whose current `state` is `approved`. Approval for an ancestor, a different change, or a
review in any other state does not satisfy `required_review`.

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

The forge capability and Postgres tests create records through both store implementations, verify
the repository foreign key, and read their versioned values back unchanged. The
`PostgresForgeScenario` case additionally exercises the Liquid forge client, cursor movement,
approval, publication, and graph evidence through a real Lake server.

## Change

Add a forge value through the store interface, both implementations, the registry, and a documented
kind in the schema constraint. The operator copies `db/schema.sql` once for this wave in its own
operator pull request, as documented in [`../control-plane/schema.md`](../control-plane/schema.md).
