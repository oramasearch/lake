# Scenario tests

These tests exercise complete user outcomes through an in-process Lake, its production Postgres
store, public HTTP routes, and public client libraries. They use `lake::test::postgres_uri()` to obtain a fresh database for each case. The caller
provides `LAKE_TEST_DB_URI` for connection settings and a role with database-creation rights;
the shared test fixture loads the schema and removes only its own database afterward.

`drive_test.cpp` covers the drive exit:

- `DriveScenario.UserSyncsSharesAndSeesHistoryEndToEnd` signs up two regular users, synchronizes a
  folder, shares it, and checks the recipient's browse API and rendered history page.
- `DriveScenario.ConcurrentOfflineEditsExposeAndResolveFirstClassConflict` synchronizes two clients,
  edits the same file while they are disconnected, checks both named files, inspects the full
  conflict record and rendered explanation, then resolves the record through the public API.
- `DriveScenario.ReplacingAnUploadedFileKeepsThePreviousVersionRestorable` (lake#123) uploads the
  same file twice through `/v1/upload_drive_files`, then checks that `drive_history` and the
  rendered history page show the replacement as a modification with both versions comparable and
  restorable, that a restore is a stable-node modification, and that a real deletion stays distinct.
  The same exercise (`../drive_history_support.hpp`) runs against the memory store's space store and
  a switched workspace store in `WorkspaceStoreDrive.*` (`../test_workspace_store.cpp`).
- `DriveScenario.TheFirstUploadCreatesMainAndAMissingFolder` and
  `DriveScenario.AnUploadIntoAMissingFolderCreatesIt` (lake#560) upload as the signed-in owner into
  a repository with no `main` and into a folder that does not exist (`Reports/2026`), and check that
  `main` and both folders are created and that an upload under a file is refused. The same
  exercise runs on the memory store, the account store and a switched workspace store.
- `DriveScenario.SyncedFolderPullsAsRepositoryAndBuildsProject` synchronizes a CMake project,
  pulls and materializes the same repository with Liquid's public client, and builds and runs it.

`SharedLinkScenario.*` in `drive_test.cpp` signs up a regular owner and syncs a private repository.
It opens the share without a session, traverses a child folder, previews supported text, checks
unsupported-format fallback, compares text/binary/empty downloads, verifies REST byte delivery,
and refuses broad drive access. It covers both link roles, guessed sibling/parent and malformed
paths, a descendant with inheritance disabled, expired links, and revocation after listing. Links
live in a workspace Zetta (lake#350, `RecordWorkspaces`); the sharing page names the workspace that
holds the links, redirects to the space's only workspace, and asks when there are several. A failed
setup sync reports liquid's error, so a Linux CI failure there names its cause (lake#547).

The drive scenario resolves the personal customer created by sign-up. Customer creation no longer
requires a separate operator bootstrap; its existing synchronization and sharing assertions remain.
See [`../../docs/api/provisioning.md`](../../docs/api/provisioning.md).

The drive scenario creates its repository in a Ready workspace of the space (lake#315), and that is
the workspace the sharing tests use.
