# Portable repository export and import

**What.** `export_repository` returns a self-contained `lake_export/1` JSON archive for one
repository. `import_repository` verifies that archive and recreates it under a named destination
customer. The archive carries liquid history and object data through the repository-store portable
transfer, the #166 artifact/attestation enumeration used for verification, forge records, direct
repository grants, and an optional Git projection.

**Why.** Git history alone does not retain review, release, webhook, permission, or deployment
facts. A lake archive makes a self-hosted destination and another lake portable without choosing a
forge-specific database format.

## Format and integrity

The top-level value has `format: "lake_export/1"`, a `payload`, a
`lake_export_manifest/1` manifest, and a TAAP/1 Ed25519 signature. The manifest has a SHA-256
digest for each payload member: `repository`, `history`, `artifacts`, `forge`, `grants`, and
`git`. The signature includes the exporting lake identity document, so a destination verifies it
without a source-database connection. Import rejects a missing member, changed member digest, or
invalid signer signature before it creates a repository.

Liquid history is transferred through the `SyncStore` portable operation. Its implementation uses
liquid's cursor-bounded `export_drips`/`import_drips` metadata contract and transfers every
reachable content-addressed object. liquid verifies every object digest and signed drip/change
during import. The archive records #166's result, attestation, trace, and SBOM enumeration as
artifact evidence alongside the restored history.

`payload.history` (`liquid_portable_history/1`) holds `items`, the signed drips, changes,
directories and references in liquid's sync shape, then `roots` and `objects`. An item's
`reference` is the change it names as a liquid digest (`blake3:<hex>`), or `""` when it names none,
as every drip, change and directory item does (lake#776). Until lake#776 an item that named no
change was written as `unknown:` and 64 zeros, the text liquid gives its empty value, which no
digest parser accepts, so `import_repository` refused every archive `export_repository` had made
with `invalid_input` "invalid sync item reference". Import reads both spellings as the empty
value, so an archive made before the fix imports too.

When `include_git` is true, `git` contains the loose-object Git repository emitted by liquid's
`export_git`, encoded as path/content pairs, its selected ref, and any Git fidelity degradations.
An import into lake uses the liquid portable history as authoritative; the Git projection is for a
Git consumer and is still manifest-verified.

Forge records retain their current versioned values. Import creates new local identifiers and
rewrites issue-comment, review-comment, and webhook-delivery parent references to those new
identifiers. Direct grants are recreated on the destination repository policy object. Archive
records themselves are not copied, preventing an export from recursively containing prior
exports.

## API

`POST /v1/export_repository` accepts `repository_id` and optional `include_git`. It requires
repository read access and returns `{archive, record}`. The durable `export` record retains the
manifest and whether Git was requested; the returned archive is the download payload.

`POST /v1/import_repository` accepts `customer_id`, `workspace_id`, destination `name`, and
`archive`. It requires write access to the destination customer. The new repository belongs to
`workspace_id`, a live workspace of that customer's account (lake#315); the archive's own
`workspace_id` records where it came from and is never reused. A missing or unknown workspace is
`invalid_input` at `/workspace_id`. The source description and visibility are retained. A destination name collision or a failed liquid transfer leaves the import refused.

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The API fixture exports a repository with an issue, review, release, and direct grant into a
second control plane, verifies the manifest signature, and compares the portable history,
artifacts, forge values, and grants. It also changes a payload member and proves import rejects
the digest mismatch. That fixture uses a fake repository store, so it never wrote a real history
item; `WorkspaceStoreExport.AnArchiveExportJustMadeImportsIntoTheSpaceStoreAndAWorkspaceStore`
(`tests/test_workspace_store.cpp`, lake#776) runs both capabilities over lake's real liquid store.
It pushes a history with liquid, writes a binary file, an empty file and an edit, deletes the empty
file with `commit_repository_files`, exports, then imports the archive into the account store and
into a switched workspace store, and compares the tree and the edited file.

## Change

Add a new archive member in the export payload, manifest constructor, and import verifier in the
same change. A member must have a stable documented JSON shape, a manifest digest, and a
round-trip comparison in the export fixture.
