# Build and verification

[local.md](local.md) documents native configuration and execution. [testing.md](testing.md)
describes isolated filesystem and PostgreSQL fixtures. [session-authentication.md](session-authentication.md)
records the session lookup measurements, row-access regression and lifecycle proof.

Run static and shared suites with the local PostgreSQL fixture, one suite at a time and builds
limited to six jobs. Keep each proof page aligned with the tests it names.

Lake's targets and test binaries build with `-Werror=return-type`, so a `liquid::runtime::task<T>`
coroutine cannot flow off its end without a result; [local.md](local.md) says why (lake#476).

The native Zetta client target includes the authorized checkout's runtime event support required
by its HTTP transport; [local.md](local.md) records this without changing the bridge pin.

The session authentication proof records successful complete static and shared PostgreSQL suites,
the intentionally failing baseline growth assertions, and the operator migration prerequisite.
