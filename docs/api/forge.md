# Forge records

**What.** Migration `0012_forge_export_import` persists forge, export, and GitHub-import facts in
`forge_records`. Each row belongs to a repository, has a constrained kind, and retains a versioned
JSON object as its value. The API capabilities that create and read these facts are added with the
forge surface.

**Why.** A review anchor is an evolving value: it initially holds `{change, path, line, column}`
and the temporary mapper adds its disposition when a later change is selected. Liquid #174 will
replace that mapper with `repository::map_anchor` without a database migration or a reinterpretation
of existing review comments.

## Stored kinds

`issue`, `issue_comment`, `review`, `review_comment`, `release`, `webhook`, `webhook_delivery`,
`branch_protection`, `export`, and `github_import` are the allowed values. The repository foreign
key and the kind constraint are enforced by Postgres; both store implementations require an object
value and a known repository.

Each record has a UUID and creation/update timestamps. Values are intentionally immutable facts
where possible; state transitions retain the same record identifier and replace only its versioned
value. Secrets such as webhook signing material are never exposed in a list or export result.

## Issues and reviews

`create_issue` creates an open issue with Markdown `body`, labels, assignees, and explicit
references. A reference is an object such as `{ "change": "..." }` or `{ "path": "..." }`.
`update_issue` changes its fields or its state (`open` or `closed`), and `create_issue_comment`
adds Markdown discussion after confirming that the issue belongs to the repository.

`create_review` binds an open review to exactly one liquid `change_id`. `update_review` transitions
that review to `approved` or `closed`. `create_review_comment` verifies that its parent review is
in the same repository and accepts an anchor value with required `change`, `path`, `line`, and
`column` members.

`map_review_anchors` is the provisional adapter for liquid #174. It receives a source change,
target change, path, and the text hunks between them. It updates only matching comment anchors and
reports `same`, `shifted`, `approximate`, or `deleted`. Once liquid provides
`repository::map_anchor`, the adapter will call it without changing persisted comments.

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

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The forge capability and Postgres tests create records through both store implementations, verify
the repository foreign key, and read their versioned values back unchanged.

## Change

Add a forge value through the store interface, both implementations, the registry, and a documented
kind in the schema constraint. The operator copies `db/schema.sql` once for this wave in its own
operator pull request, as documented in [`../control-plane/schema.md`](../control-plane/schema.md).
