# Image builds

Lake exposes customer-scoped, audited image builds through the same capability registry as REST,
OpenAPI and MCP. A request records intent and returns the build ID; the operator runs one isolated
Job and reports status through the [build row](../control-plane/builds.md).

## Capabilities

All build capabilities require `workspace_id`, the owning workspace's `workspaces.id`.
Creation persists it immutably; missing, unknown or foreign IDs return `invalid_input`.
App builds use the app's same workspace. Event publication, reads and logs use that workspace's
`build-events.<id>`, retained for 24 hours/5,000 events and reaped after the terminal 24-hour
window. Reads never recreate a reaped topic. The operator must pass the ID explicitly.

| Capability | Input | Result |
|---|---|---|
| `build_image` | customer, repository, revision, optional context/Dockerfile/build args, target `name:tag` | queued build ID |
| `build_app` | customer, App ID, optional `source {repository, revision, path?}` (required once the App runs a built image, since that deploy drops its stored source) | queued image build `FROM` the configured app-runtime image (`LAKE_APPS_RUNTIME_IMAGE`); the app's `APP_DEPS` are installed into `/app/.venv` with the base image's system pip (as root, globbing off), then the image runs as uid 10001 |
| `get_build`, `list_builds` | customer and optional build ID | desired state, phase, digest, public pull image, size and timestamps |
| `cancel_build` | customer and build ID | desired state becomes `cancelled` |
| `publish_build_event` | build ID plus phase/log/done/error fields | retained event ID |
| `receive_build_events` | customer, build ID, optional one-based sequence and wait | ordered events and next cursor |
| `build_logs` | customer and build ID | retained log lines and their events |

Policy resolves every customer before the handler runs. The repository must be live and owned by
that customer. A build ID belonging to another customer is reported as absent after policy has
authorized the requested customer. Every call uses the signed registry audit path; credential
hashes and plaintext credentials are excluded from capability output and audit details.

A workspace methodology's image (lake#655) is a `build_image` Lake queues itself for Validate and
runs: an inline Dockerfile `FROM ${METHODOLOGY_RUNTIME_IMAGE}` (`LAKE_METHODOLOGY_RUNTIME_IMAGE`)
that copies the methodology's folder to `/methodology` and installs its lock file with hashes
required. `METHODOLOGY_RUNTIME_IMAGE` is in the build-argument allow-list beside `APP_RUNTIME_IMAGE`
([methodologies.md](methodologies.md)).

`build_app` requires `LAKE_APPS_RUNTIME_IMAGE`, which the operator sets from the Lake's
`spec.apps.runtimeImage`. Lake stores a generated Dockerfile beginning with
`ARG APP_RUNTIME_IMAGE` and `FROM ${APP_RUNTIME_IMAGE}`, and stores the configured value in the
build row's `APP_RUNTIME_IMAGE` argument. An absent setting returns `store_unavailable` naming
`LAKE_APPS_RUNTIME_IMAGE`; Lake does not substitute an unqualified image name.

## Source and progress contract

The source credential may call:

```text
GET /v1/repository_tree?repository=<id>&revision=<revision>&recursive=true
GET /v1/repository_file?repository=<id>&revision=<revision>&path=<path>
Authorization: Bearer <source token>
```

Lake accepts those builder names as aliases for the capability's `id` and `ref` fields. The tree
response is `value.items`; file results include UTF-8 `contents` and a base64 form for arbitrary
bytes.

The builder and operator post to `publish_build_event`; Lake publishes the body to the retained
Zetta topic `build-events.<build-id>`. Allowed types are `phase`, `log`, `done` and `error`. A post
may carry phase, step/steps, up to the 64 KiB request bound of batched lines, digest, size and
message. Delivery is best-effort for the Job. `receive_build_events` follows the topic with the
same one-based cursor contract as agent events, and `build_logs` reconstructs the retained log
lines after the pod is gone.

## Image names and network path

The customer supplies only `name:tag`. Lake expands it to:

```text
<LAKE_INTERNAL_URL authority>/<customer handle>/<name>:<tag>
```

The authority omits the scheme's default port (`http://lake-prod.lake.svc:80` gives
`lake-prod.lake.svc`). BuildKit matches its registry configuration against the reference's host
exactly, and the builder marks that host plain HTTP; a reference spelled `host:80` never matched,
so pushes went out over HTTPS and failed.

Before success, build JSON reports `image: null`. After the operator reports `Succeeded` with a
digest, `get_build` and `list_builds` replace the internal authority and mutable tag with the
configured `LAKE_PUBLIC_HOST` and immutable digest:

```text
<LAKE_PUBLIC_HOST>/<customer handle>/<name>@<digest>
```

The stored `target` remains the tagged in-cluster push reference. Application Agent and Ship → Apps
deploy the returned `image`, so Kubernetes nodes pull through the public Lake host.

Builders push blobs and the registry cache over the in-cluster Service named by
`LAKE_INTERNAL_URL`. Container layers exceed the public Ingress request-body limit, while the
Service reaches the same Lake registry without that proxy. The cache is
`<internal authority>/<space>/cache/<name>` and uses the build's same scoped credential.

Lake admits OCI `PATCH` and final `PUT` upload requests up to
`LAKE_MAX_REGISTRY_BLOB_BYTES`, which defaults to 4 GiB. The adapter appends each incoming buffer
to the upload file instead of retaining the layer in memory. `PATCH` accepts the Distribution
`Content-Range: START-END` form (and the equivalent `bytes START-END/TOTAL` form), requires the
start to equal the current upload size, and verifies that the declared range matches the bytes
received. The final `PUT` may append its own body or be empty after one or more patches. Completion
hashes the upload from disk, checks the requested digest, and then publishes the file as the blob.

An upload that would exceed the configured limit returns HTTP 413 with OCI code
`BLOB_UPLOAD_INVALID` and names the byte limit. Invalid ranges and interrupted reads leave the
previous completed prefix available for retry. The larger parser allowance does not apply to
capability JSON, forms, model or dataset uploads, drive uploads, MCP, or other request bodies;
those retain the 64 MiB adapter limit described in [the HTTP contract](../serving/http.md).

Build images are private to their account. OCI push credentials may read and write only below their
own `<space>/` prefix. Customer pull credentials may read only that account. Existing legacy
published packages remain anonymous reads. Registry routes preserve one-, two- and three-part OCI
repository names so the account prefix and internal cache namespace remain part of authorization.

## Browser pages

An authorized repository page posts its **Build image** action through the same `build_image`
capability and redirects to Ship → Packages. That page lists the selected account's rows beside the
pushed image names, filters images to accounts the caller can read, and hides internal cache
namespaces. `/ship/app?customer_id=<id>&app_id=<app-id>` shows an App's build history and queues
`build_app`. Both pages long-poll `receive_build_events`, advance the retained sequence cursor,
and append phase and log events without giving the browser bus credentials.

## Verify and change

Run `Builds.*`, `PostgresBuilds.*`, `RegistryProtocols.*` and the HTTP server suite in both native
suites. The build tests exercise asynchronous row creation, runtime-image configuration, missing
configuration, fake operator feedback, cancellation, credential rotation and retained events. The
HTTP test supplies the runtime image through real `ServerOptions` and checks the queued build row.
The registry tests prove Basic and Bearer build-push acceptance, app Basic pull access to only its
exact repository, 403 `DENIED` outside each credential boundary, a layer larger than 64 MiB through
the real server, ranged patches followed by an empty final put, and configured-limit errors
without invoking a container engine. The HTTP suite separately proves that an oversized
non-registry JSON body remains rejected.

Keep capability JSON in `src/api/build_capabilities.cpp`, native registry translation in the one
HTTP adapter, and durable ownership in `docs/control-plane/builds.md`.
