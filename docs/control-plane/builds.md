# In-cluster build rows

Lake owns build intent in migration `0036_builds`; the Orama operator owns Kubernetes execution.
Each request inserts one row and returns its UUID immediately. Lake never creates a pod or waits
for the Job inside the request.

## Row ownership

The row is the exact contract consumed by orama-operator#195:

> `id, customer_id, repository, revision, context, dockerfile, target, build_args_json, desired,
> phase, digest, size, message, token_hashes_json, created_at, updated_at`.

Lake writes the source, target, arguments and `desired`. The operator writes `phase`, result fields
and `token_hashes_json`. A desired insert or cancellation sends `NOTIFY lake_builds, '<build id>'`;
the operator's ordered poll remains the recovery path when a notification is lost.

`desired` is `queued` or `cancelled`. Observed `phase` is `Queued`, `Running`, `Succeeded`,
`Failed` or `Cancelled`. Cancellation retains the row and asks the operator to remove only the Job
and Secret owned by that build. Terminal rows retain digest, compressed registry size, message and
timestamps for history.

## Credentials

The operator mints three independent 256-bit credentials and stores only their SHA-256 values in
the row:

| Purpose | Lake permits |
|---|---|
| `source` | `repository_tree` and `repository_file` for the row's repository and revision |
| `push` | OCI push and cache pull below the target space prefix |
| `events` | `publish_build_event` for this build ID |

The operator creates a suspended Job, writes all three hashes, and releases the Job only after a
later read sees those hashes. Lake re-reads the row on every authenticated call, so cancellation,
terminal status or hash rotation revokes a reused request context. Plain tokens exist only in the
operator-owned build Secret and never enter Postgres, audit details, event payloads or Lake logs.

## Job input

`target` in the row is `<in-cluster-registry-host>/<space>/<name>:<tag>`. `build_args_json` is an
object of string values and Lake accepts the builder allow-list `PYTHON_VERSION`, `APP_DEPS` and
`APP_RUNTIME_IMAGE`. `context` and a path-valued `dockerfile` are repository-relative and cannot
traverse `..` or `.git`; a generated Dockerfile is stored as `inline:<contents>`.

`build_app` takes a runtime App's repository, revision and source path. It queues an image target
named for the App. When `APP_DEPS` is present in the App environment, the generated
`FROM app-runtime` Dockerfile installs those dependencies before copying the application. A
successful image remains an ordinary build result that can be supplied to `deploy_app`; the build
row deliberately does not change App desired state before the image exists.

## Schema rollout

`builds_guard` is the newest guard. The operator must vendor `db/schema.sql` byte-for-byte and
apply it before builds are enabled. The migration is idempotent and adds customer/time and
purpose-hash indexes. Lake pods do not run migrations.

## Verify and change

`Builds.*` covers memory desired state, fake operator status, cancellation, token purpose and event
retention. `PostgresBuilds.*` covers the same row shape, `lake_builds` notifications and hashed
credential lookup against Postgres. Run both native static and shared suites with
`LAKE_TEST_DB_URI` set.

When the Job contract changes, update this page, `docs/api/builds.md`, the schema and the operator's
row adapter together. Do not add plaintext credential columns or make Lake own Kubernetes work.
