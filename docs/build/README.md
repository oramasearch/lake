# Build and verification

[tunnel-grants-proof.md](tunnel-grants-proof.md) records the native credential, migration,
provider and transport proof for lake#730 and the coordinator's deferred relay checks.

[testing.md](testing.md) documents `LAKE_TEST_SKIP_STALE_SWEEP=1` for shared hosts where a run
must leave earlier runs' databases alone. [Network verification](../control-plane/network.md#verify)
names the PostgreSQL-backed control-plane proof.

[local.md](local.md) documents native configuration and execution. [testing.md](testing.md)
describes isolated filesystem and PostgreSQL fixtures. [session-authentication.md](session-authentication.md)
records the session lookup measurements, row-access regression and lifecycle proof.
[ci.md](ci.md) documents the `ci` workflow: the full suite on Linux for every pull request and push
to `main`, with a Postgres service, a cached Zetta binary for the real-Zetta tests and the token
gates (lake#547).

Run static and shared suites with the local PostgreSQL fixture, one suite at a time and builds
limited to six jobs. Keep each proof page aligned with the tests it names.

Lake's targets and test binaries build with `-Werror=return-type`, so a `liquid::runtime::task<T>`
coroutine cannot flow off its end without a result; [local.md](local.md) says why (lake#476).

Threads lake starts are `lake::Thread`s with an explicit 8 MiB stack, so a Debug build on macOS
does not overflow the platform's 512 KiB default; [local.md](local.md#debug-builds-and-thread-stacks)
records the measured peaks (lake#478).

The native Zetta client target includes the authorized checkout's runtime event support required
by its HTTP transport; [local.md](local.md) records this without changing the bridge pin.

The session authentication proof records successful complete static and shared PostgreSQL suites,
the intentionally failing baseline growth assertions, and the operator migration prerequisite.
