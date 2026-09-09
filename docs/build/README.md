# Build and verification

[local.md](local.md) documents native configuration and execution. [testing.md](testing.md)
describes isolated filesystem and PostgreSQL fixtures. [session-authentication.md](session-authentication.md)
records the session lookup measurements, row-access regression and lifecycle proof.

Run static and shared suites with the local PostgreSQL fixture, one suite at a time and builds
limited to six jobs. Keep each proof page aligned with the tests it names.

The session authentication proof records successful complete static and shared PostgreSQL suites,
the intentionally failing baseline growth assertions, and the operator migration prerequisite.
