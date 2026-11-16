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
invalid signer signature before it creates a repository. The signer's `taap_id` must exactly
equal `taap:1:ed25519:` followed by the uppercase, unpadded base32 encoding of the SHA-512/256
digest of its decoded Ed25519 public key, the same derivation Lake uses to mint its identity.
Changing that identity to another instance's name and re-signing with a fresh key is refused
with `invalid_signature` (lake#954). Rotation mints a new key-derived name and remains
portable.

This verifies the archive's consistency and possession of its signing key. It does not establish
trust in the exporting instance or authenticate the people or builders named in its metadata.
Lake accepts self-contained archives without a peer trust registry, and marks all imported
provenance `untrusted`, including an archive from an instance the importer recognizes.

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

Forge records retain their historical values with an `import_provenance` object containing
`source_signer`, `trust: "untrusted"`, and `imported_by`. Supplied provenance markers are replaced.
Supplied `author` and `opened_by` fields move to `import_provenance.source_author` and
`source_opened_by`. Local issues and issue comments name the importer as `author`, and reviews
name the importer as `opened_by`; other copied author fields are removed. Imported items now
show the importer as author in the console. Historical authors and issuers remain source claims.
Import creates new local identifiers and rewrites issue-comment, review-comment, and
webhook-delivery parent references to those new
identifiers. Direct grants are recreated on the destination repository policy object with the
importer as `granted_by_taap_id` and a new Lake-signed `lake.grant/1` attestation. Source grantors
and attestation digests never become local grant evidence. Their signed local attestations are
recorded in the import audit as `grant_attestations`. Archive records themselves are not copied,
preventing an export from recursively containing prior
exports.

## What an archive never carries

Export needs only read on the repository, so the archive carries nothing a reader cannot read
otherwise (lake#933, lake#1314):

- **No webhook signing secret.** A `webhook` record keeps its `url` and `events` and loses
  `secret`, as `list_webhooks` answers it. Whoever held the secret could sign deliveries the
  receiver accepts as lake's.
- **Webhooks and their deliveries only for a writer.** `list_webhooks` and
  `list_webhook_deliveries` need `write`, so the `webhook` and `webhook_delivery` records are in
  `forge` only when the caller has write on the repository (the operator credential included); a
  reader's archive has neither. A receiver URL is often a credential itself (a chat incoming-webhook
  URL, a CI trigger with a token in its path or query), and a delivery's `payload` and `signature`
  replay a real signed event. An import of a reader's archive therefore creates no webhook.
- **Grants only for an administrator.** `list_grants` needs `admin`, so `grants` is the
  repository's direct grants only when the caller has admin on the repository (the operator
  credential included); for any other caller it is an empty array.

Import gives a webhook that arrives without a secret a new random one that nobody holds, so its
deliveries are signed but never verify at the old receiver. To deliver again, create the webhook
on the new repository with the receiver's secret. An archive made before this change still carries
its secrets and keeps them on import.

## API

`POST /v1/export_repository` accepts `repository_id` and optional `include_git`. It requires
repository read access and returns `{archive, record}`, with webhook secrets left out, webhooks
and deliveries only for a repository writer and grants only for a repository administrator (see
above). The durable `export` record retains the
manifest and whether Git was requested; the returned archive is the download payload.

`POST /v1/import_repository` accepts `customer_id`, `workspace_id`, destination `name`, and
`archive`. It requires write access to the destination customer. The new repository belongs to
`workspace_id`, a live workspace of that customer's account (lake#315); the archive's own
`workspace_id` records where it came from and is never reused. A missing or unknown workspace is
`invalid_input` at `/workspace_id`. The source description and visibility are retained. A destination name collision or a failed liquid transfer leaves the import refused.

A review is imported without its `approval`: an approval is lake's record of who approved
in the repository it was given in, and a signed archive proves only who exported it, so an
imported review never satisfies a protected branch's `required_review`. Its state is kept as
history. See [forge](forge.md#who-may-approve).

Imported artifact evidence also carries `import_provenance`; its `verification` is `untrusted`
and its attestations have `verified: false`. A release's carried `artifacts` move under
`import_provenance.artifacts`, leaving its active artifact list empty, so source evidence cannot
satisfy a destination branch's verified-release requirement. Liquid's own history and object
verification still run independently. The import audit records the bound `source_signer` and
`source_trust: "untrusted"`.

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The API fixture exports a repository with an issue, review, release, and direct grant into a
second control plane, verifies the manifest signature, and compares the portable history,
artifacts, forge values, and grants. It also changes a payload member and proves import rejects
the digest mismatch. It re-signs an archive with another key while keeping the claimed source
identity, proves refusal before any destination repository or transfer is created, and checks
local grant evidence and untrusted forge/artifact markers. The direct
`Export.ManifestSignerMustBeNamedAfterItsSigningKey` case covers valid and rotated keys, false
identifiers, malformed identities, and a signature made by the wrong key.
That fixture uses a fake repository store, so it never wrote a real history
item; `WorkspaceStoreExport.AnArchiveExportJustMadeImportsIntoTheSpaceStoreAndAWorkspaceStore`
(`tests/test_workspace_store.cpp`, lake#776) runs both capabilities over lake's real liquid store.
It pushes a history with liquid, writes a binary file, an empty file and an edit, deletes the empty
file with `commit_repository_files`, exports, then imports the archive into the account store and
into a switched workspace store, and compares the tree and the edited file.
`Export.AnArchiveCarriesNoWebhookSecretAndGrantsOnlyForAnAdministrator`
exports a repository with a webhook and a delivery as a reader, a writer and the operator, finds the
secret nowhere in any archive, the webhook, its URL and its delivery nowhere in the reader's, and
grants only in the operator's, then imports the writer's archive and checks the webhook's next
delivery is signed neither with the source secret nor with an empty key.

## Change

Add a new archive member in the export payload, manifest constructor, and import verifier in the
same change. A member must have a stable documented JSON shape, a manifest digest, and a
round-trip comparison in the export fixture.
