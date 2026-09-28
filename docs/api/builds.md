# Image builds

Lake exposes customer-scoped, audited image builds through the same capability registry as REST,
OpenAPI and MCP. A request records intent and returns the build ID; the operator runs one isolated
Job and reports status through the [build row](../control-plane/builds.md).

## Capabilities

| Capability | Input | Result |
|---|---|---|
| `build_image` | customer, repository, revision, optional context/Dockerfile/build args, target `name:tag` | queued build ID |
| `build_app` | customer and App ID | queued app-runtime image build |
| `get_build`, `list_builds` | customer and optional build ID | desired state, phase, digest, size and timestamps |
| `cancel_build` | customer and build ID | desired state becomes `cancelled` |
| `publish_build_event` | build ID plus phase/log/done/error fields | retained event ID |
| `receive_build_events` | customer, build ID, optional one-based sequence and wait | ordered events and next cursor |
| `build_logs` | customer and build ID | retained log lines and their events |

Policy resolves every customer before the handler runs. The repository must be live and owned by
that customer. A build ID belonging to another customer is reported as absent after policy has
authorized the requested customer. Every call uses the signed registry audit path; credential
hashes and plaintext credentials are excluded from capability output and audit details.

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

The install/pull form replaces the internal authority with the configured public Lake origin:

```text
<public Lake host>/<customer handle>/<name>:<tag>
```

Builders push blobs and the registry cache over the in-cluster Service named by
`LAKE_INTERNAL_URL`. Container layers exceed the public Ingress request-body limit, while the
Service reaches the same Lake registry without that proxy. The cache is
`<internal authority>/<space>/cache/<name>` and uses the build's same scoped credential.

Build images are private to their space. OCI push credentials may read and write only below their
own `<space>/` prefix. Customer pull credentials may read only that space. Existing legacy
published packages remain anonymous reads.

## Browser pages

An authorized repository page posts its **Build image** action through the same `build_image`
capability and redirects to Ship → Packages. That page lists the selected space's rows beside the
pushed image names, filters images to spaces the caller can read, and hides internal cache
namespaces. `/ship/app?customer_id=<id>&app_id=<app-id>` shows an App's build history and queues
`build_app`. Both pages long-poll `receive_build_events`, advance the retained sequence cursor,
and append phase and log events without giving the browser bus credentials.

## Verify and change

Run `Builds.*`, `PostgresBuilds.*` and `RegistryProtocols.*` in both native suites. The build tests
exercise asynchronous row creation, fake operator feedback, cancellation, credential rotation and
retained events. The registry tests prove Basic and Bearer acceptance and 403 `DENIED` outside the
space prefix without invoking a container engine.

Keep capability JSON in `src/api/build_capabilities.cpp`, native registry translation in the one
HTTP adapter, and durable ownership in `docs/control-plane/builds.md`.
