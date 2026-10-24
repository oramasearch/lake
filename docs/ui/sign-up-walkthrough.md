# Fresh-profile sign-up walkthrough

## The journey in the web app

Without Clerk, a new person's first repository takes these steps in the web app (lake#561):

1. `/sign-up` (`frontend/src/pages/account/SignUpPage.tsx`) links **Install liquid** and the
   identity command reference, and asks for email, display name, a password of at least 12
   characters, a device name and the public identity document printed by
   `liquid identity show --document`. The private key never leaves the client.
2. **Create account** calls `sign_up_password`, which provisions the person's own account and sets
   the HttpOnly `lake_session` cookie. A malformed document is refused with the recovery message
   below and creates nothing. The page then opens `/repositories`.
3. A repository belongs to one workspace (lake#315). While the account has none, the create form
   says so and links **Workspaces**, where the person creates one (`create_workspace`).
4. **Create a Repository** on `/repositories` chooses the workspace, a name and an optional
   description, and calls `create_repository` (private). The repository home opens.

Under Clerk, `/sign-up` shows Clerk's form; the invitation sentence appears only where the
instance's sign-up mode is restricted ([front-door.md](front-door.md), lake#1116).

To verify, follow the steps in fresh browser profiles at 1280 and 640 px in both themes against a
native lake on a separate database, then check zero usage with
`GET /v1/metering_usage?customer_id=<id>` before writing any content.

## Recovery message

> TAAP identity document is incomplete or invalid canonical CBOR: declared length exceeds input. Run liquid identity show --document and paste the complete output.

Invalid alphabet input also returns a specific base64url message with the same export command.
Signatures and canonical encoding remain verified by libliquid before account creation.

## Recorded walkthrough, 2026-09-08

Historical: everything below records the server-rendered pages, and the screenshots show them.

Recorded on 2026-09-08 against native Lake with local PostgreSQL 17. Each run used a fresh
Chrome browser context and a separate client identity directory inside this worktree. The
walkthrough database (`lake_walkthrough_119`) was separate from the suite database
(`lake_test_119`). No operator bearer was supplied to any onboarding request. The existing
native Liquid binary supplied the documented identity export command.

### Follow the journey

1. Open `/sign-up` with no cookies. Follow **Install liquid** for prerequisites and the supported
   source build, or expand **Installation commands** on the page. The command sequence clones
   Liquid with submodules, configures with CMake and Ninja, builds with `-j 6`, installs to
   `$HOME/.local`, and adds its `bin` directory to `PATH`. Repository read access is required.
2. Run `liquid identity show --document`. On a fresh client profile, Liquid creates the signing
   identity locally and prints only the public document. In this proof each profile set
   `LIQUID_HOME` to its own directory. No key file was read or sent to Lake.
3. Enter email, display name, a password of at least 12 characters, and device name. Paste only
   the first 40 characters of the public document and submit to exercise recovery. The page
   returns the message below and creates no account.
4. Run the same command again and paste its complete output, without quotes. Fill the remaining
   form fields and select **Create account**. The response sets an HttpOnly session cookie and
   redirects to `/repositories`; its customer selector contains the new personal handle.
5. Before writing any repository content, call `GET /v1/metering_usage?customer_id=<id>` with that
   browser session. The customer ID is the `id` in `GET /v1/list_customers`. Confirm the hourly
   `stored_bytes`, `transfer_in_bytes`, `transfer_out_bytes`, `compute_seconds`, and `cost_micros`
   are all `0`. This run configured all four unit prices to `1000` and the storage quota to
   `1073741824` bytes, so zero cost cannot be explained by free prices. No customer quota
   override was present. Since lake#348 the report has no hourly rows before any transfer or
   build (`items` is empty), and storage is `summary.storage.repositories_bytes`, measured live;
   the recorded run and the SQL below predate that.
6. Since lake#315 a repository belongs to a workspace: an account without one is sent to
   **Workspaces** first, and the form then chooses the workspace instead of the customer. The
   recorded run below predates that. In **Create a repository**, keep the personal customer, enter `my-first-repository`, and
   optionally describe it. Select **Create repository**. The form calls the registry capability
   with private visibility and redirects to `/repository?id=<id>`. The repository home displays
   its generated customer handle, the repository name, private visibility, and inherited `admin`
   access for the client identity.
7. Repeat in fresh profiles at 1280 and 640 pixels, in light and dark. All four runs completed;
   each final page's document width matched the viewport width. The shared narrow navigation
   retains its existing horizontal scrolling behavior.

### Recorded customers and zero usage

The separate walkthrough database contains exactly four customers, four users, and four private
repositories: one for each profile. Each customer's zero usage was recorded before its first
repository was created.

| Width | Theme | Personal customer | Initial stored bytes | Initial cost (micro-units) |
|---|---|---|---:|---:|
| 1280 | light | `royal-pebble-413` | 0 | 0 |
| 1280 | dark | `ready-lynx-184` | 0 | 0 |
| 640 | light | `olive-vista-171` | 0 | 0 |
| 640 | dark | `rare-geyser-529` | 0 | 0 |

```sql
SELECT count(*) FROM customers;
-- 4
SELECT c.name, m.stored_bytes, m.transfer_in_bytes, m.transfer_out_bytes,
       m.compute_seconds
FROM customers c JOIN metering_usage m ON m.customer_id = c.id
ORDER BY c.name;
-- Four rows; every usage field is 0.
```

### Screenshots

| Profile | Sign-up instructions | Malformed document | First-repository form | Repository home |
|---|---|---|---|---|
| 1280-light | [Instructions](screenshots/sign-up/1280-light-sign-up.png) | [Recovery](screenshots/sign-up/1280-light-malformed.png) | [Form](screenshots/sign-up/1280-light-repositories.png) | [Repository](screenshots/sign-up/1280-light-first-repository.png) |
| 1280-dark | [Instructions](screenshots/sign-up/1280-dark-sign-up.png) | [Recovery](screenshots/sign-up/1280-dark-malformed.png) | [Form](screenshots/sign-up/1280-dark-repositories.png) | [Repository](screenshots/sign-up/1280-dark-first-repository.png) |
| 640-light | [Instructions](screenshots/sign-up/640-light-sign-up.png) | [Recovery](screenshots/sign-up/640-light-malformed.png) | [Form](screenshots/sign-up/640-light-repositories.png) | [Repository](screenshots/sign-up/640-light-first-repository.png) |
| 640-dark | [Instructions](screenshots/sign-up/640-dark-sign-up.png) | [Recovery](screenshots/sign-up/640-dark-malformed.png) | [Form](screenshots/sign-up/640-dark-repositories.png) | [Repository](screenshots/sign-up/640-dark-first-repository.png) |

### Native proof

Run the full suite with `LAKE_TEST_DB_URI` pointing at a fresh schema-loaded database and
`LAKE_INSTANCE_KEK` set. Use a separate database for a manual server; test fixtures own their
instance identity and must not share that database with a server using another key.

```sh
DB=lake_test_119
createdb -h 127.0.0.1 -U stevesperandeo "$DB"
psql -h 127.0.0.1 -U stevesperandeo -d "$DB" -f db/schema.sql
export LAKE_TEST_DB_URI="postgresql://stevesperandeo@127.0.0.1:5432/$DB"
export LAKE_INSTANCE_KEK=$(openssl rand -hex 32)
mkdir -p .cache/issue-119/tmp
export TMPDIR="$PWD/.cache/issue-119/tmp"
python3 -m venv .cache/test-python
.cache/test-python/bin/python -m pip install jsonschema
cmake -S . -B build -G Ninja \
  -DPython3_EXECUTABLE="$PWD/.cache/test-python/bin/python" \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build -j 6
ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON \
  -DPython3_EXECUTABLE="$PWD/.cache/test-python/bin/python" \
  -DLAKE_ZETTA_SOURCE_DIR=/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
cmake --build build-shared -j 6
DB=lake_test_119_shared
createdb -h 127.0.0.1 -U stevesperandeo "$DB"
psql -h 127.0.0.1 -U stevesperandeo -d "$DB" -f db/schema.sql
export LAKE_TEST_DB_URI="postgresql://stevesperandeo@127.0.0.1:5432/$DB"
ctest --test-dir build-shared --output-on-failure
```

The branch was rebased onto `78bdf57` (registry schema conformance). Because that change touched
a public header and a CMake list, both build directories were removed and configured from
scratch, submodules were refreshed, and both full suites were rerun. Final summaries follow.

```text
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 225

Total Test time (real) = 397.07 sec

$ ctest --test-dir build-shared --output-on-failure
100% tests passed, 0 tests failed out of 225

Total Test time (real) = 373.51 sec
```

Both runs skipped `HuggingFaceClient.CommonFlowsRunWhenPythonClientIsInstalled` because the
optional Python client is absent, and retained the disabled
`SearchBenchmark.BuildAndQueryTenThousandFiles` benchmark. No PostgreSQL-backed test was
skipped. All registered schema-conformance, UI-token, and contrast checks passed.

Linux proof deferred: the designated `tools/linux-proof.sh` helper is absent on this host.
No image build was attempted; no Docker on this host.

Browser validation also accepted `my-first-repository` and `a`, and refused `Bad Name`,
`-bad`, and `bad-` with the same handle rules enforced by the registry.

The homepage and navigation work remain #135. MCP schema conformance landed separately in #136
and is included in the final suites.
