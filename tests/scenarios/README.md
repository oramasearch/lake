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
