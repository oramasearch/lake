# Artifact registry protocols

## What

Lake exposes native HTTP endpoints for OCI Distribution, npm, the Python Simple Repository API,
and debuginfod. These are protocol routes rather than JSON capabilities: existing package clients
send and receive their native metadata and binary payloads without a translation step. Writes
require the configured Lake administrator bearer. Package metadata, blobs, wheels, and debug
artifacts are public reads after publication.

For server `node_name = <instance>`, registry objects live below:

```text
LAKE_STORE_ROOT/<instance>/registry/blobs/
LAKE_STORE_ROOT/<instance>/registry/manifests/
LAKE_STORE_ROOT/<instance>/registry/npm/
LAKE_STORE_ROOT/<instance>/registry/python/
LAKE_STORE_ROOT/<instance>/registry/debug/
```

Writes use a sibling temporary file followed by rename. OCI upload sessions are staged separately;
completion recalculates SHA-256 and refuses bytes that do not match the requested digest. A
manifest is accepted only when it is schema version 2 JSON and every referenced config or layer
digest is already present.

## Protocol routes

| Protocol | Routes |
|---|---|
| OCI Distribution | `GET /v2/`; `POST /v2/:repository/blobs/uploads/`; `PATCH` or `PUT /v2/:repository/blobs/uploads/:upload`; `GET` or `HEAD /v2/:repository/blobs/:digest`; `GET`, `HEAD`, or `PUT /v2/:repository/manifests/:reference` |
| npm | `GET` or `PUT /:package`; `GET /:package/-/:filename` |
| Python | `POST /legacy/`; `GET /simple/:package/`; `GET` or `HEAD /packages/:filename` |
| debuginfod | `GET /buildid/:build_id/executable`; `GET /buildid/:build_id/debuginfo`; `GET /buildid/:build_id/source/:path` |
| Lake debug publication | `PUT /_lake/debug/:build_id/:kind`; source publication adds `?source=<path>` |

OCI responses include `Docker-Distribution-Api-Version`, upload locations, and
`Docker-Content-Digest` where the Distribution API requires them. Blob and package downloads
declare the exact stored length and do not re-encode the payload.

npm publication accepts the standard metadata document and `_attachments` tarball emitted by
`npm publish`. Lake removes the inline attachment from stored metadata and rewrites the version's
tarball URL to its own origin. Python publication accepts the legacy multipart upload used by
package publishers, normalizes project names using the Python repository rule, and renders a PEP
503-compatible project page whose links carry a SHA-256 fragment.

The debuginfod read paths follow the standard build-id protocol. Lake's protected publication
path stores an executable, separate debug information, or a named source file for the same build
identifier.

## What the scenario proves

`RegistryProtocols.OciClientPushesAndPullsTwoLayersByteIdentical` uses a small Distribution API
client, not a container CLI. It uploads a config and two distinct binary layers, publishes an OCI
image manifest, pulls the manifest and both layers, and compares every returned byte.

`RegistryProtocols.NpmPublishesAndInstallsWithTheRealClient` runs the host `npm` binary to publish
and then install a fixture package through Lake. If npm is unavailable, the same test exercises
metadata and attachment publication directly. `RegistryProtocols.PythonPackagePublishesAndInstallsWithPip`
publishes a valid wheel through the repository protocol and uses the host `pip3` binary to resolve,
hash-check, download, and install it; a direct protocol download is the fallback when pip is absent.

`RegistryProtocols.DebuginfodServesExecutableDebuginfoAndSource` publishes the CMake-built
`lake-debug-fixture` executable, a debug-information payload, and its source, then retrieves all
three through debuginfod paths and compares exact bytes.

## How to verify

```sh
cmake --build build -j 6
./build/tests/lake-tests --gtest_filter='RegistryProtocols.*'
```

The executable tests require no container daemon or external registry. They use loopback HTTP and
process-unique storage directories.

## How to change

Keep protocol parsing in `src/api/http_server.cpp`, the only HTTP adapter translation unit. A new
write must authenticate before reading or publishing its payload, validate every path component,
and publish content atomically. Preserve native client response headers and exact binary bytes.
Document and test each added client operation here.
