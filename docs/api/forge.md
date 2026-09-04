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
