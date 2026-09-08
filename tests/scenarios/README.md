# Scenario tests

These tests exercise complete user outcomes through an in-process Lake, its production Postgres
store, public HTTP routes, and public client libraries. They use the caller-provided
`LAKE_TEST_DB_URI`; the proof runner owns database creation and schema loading.

`drive_test.cpp` covers the drive exit:

- `DriveScenario.UserSyncsSharesAndSeesHistoryEndToEnd` signs up two regular users, synchronizes a
  folder, shares it, and checks the recipient's browse API and rendered history page.
- `DriveScenario.ConcurrentOfflineEditsExposeAndResolveFirstClassConflict` synchronizes two clients,
  edits the same file while they are disconnected, checks both named files, inspects the full
  conflict record and rendered explanation, then resolves the record through the public API.
- `DriveScenario.SyncedFolderPullsAsRepositoryAndBuildsProject` synchronizes a CMake project,
  pulls and materializes the same repository with Liquid's public client, and builds and runs it.

`SharedLinkScenario.*` in `drive_test.cpp` signs up a regular owner and syncs a private repository.
It opens the share without a session, traverses a child folder, previews supported text, checks
unsupported-format fallback, compares text/binary/empty downloads, verifies REST byte delivery,
and refuses broad drive access. It covers both link roles, guessed sibling/parent and malformed
paths, a descendant with inheritance disabled, expired links, and revocation after listing.

The drive scenario resolves the personal customer created by sign-up. Customer creation no longer
requires a separate operator bootstrap; its existing synchronization and sharing assertions remain.
See [`../../docs/api/provisioning.md`](../../docs/api/provisioning.md).
