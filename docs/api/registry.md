# Artifact registry protocols

## What

Lake exposes native HTTP endpoints for OCI Distribution, npm, the Python Simple Repository API,
and debuginfod. These are protocol routes rather than JSON capabilities: existing package clients
send and receive their native metadata and binary payloads without a translation step. Legacy
one-component OCI repositories, package metadata, wheels, and debug artifacts remain public reads
after publication. npm, Python, debug, and legacy OCI writes require the configured administrator
bearer. Tenant OCI repositories use the scoped credentials described below.

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
digest is already present. Blob and digest-manifest reads also require that a tag in the requested
repository references the object, so a known tenant digest cannot be read through a public alias.

## The Packages page

`GET /ship/packages` (signed in; Ship in the sidebar) lists what the registries hold, read from the
storage layout above: OCI repositories with their tags, npm packages with the versions in their
metadata, and Python projects with the versions of their files. Each row carries the install
command for this lake's public origin (`LAKE_EXTERNAL_ORIGIN`, else `https://LAKE_PUBLIC_HOST`).
The page shows public packages and only those `<space>/...` images the signed-in viewer may read.
It also shows the selected space's asynchronous builds and follows their retained progress topic
for live phase and log updates.

## Tenant OCI credentials

A build push token is accepted as either `Bearer <token>` or HTTP Basic user `lake` with the token
as its password. Lake re-hashes it, resolves the active build row, and permits blob, manifest, and
cache reads or writes only below that build customer's `<space>/` prefix. A valid credential used
against another prefix receives OCI `403 DENIED`. Cancellation, terminal status, or hash rotation
revokes it.

A customer account token attenuated to `registry_pull` may read private images only in spaces its
identity may read; the operator can deliver that credential as an `imagePullSecret`. Anonymous
requests to tenant paths are refused. The administrator bearer remains valid across repositories.

Deleting a workspace removes its tenant images (lake#514, [workspaces.md](workspaces.md#deletion)):
the tags and manifest pointers of `<space>/<name>` and `<space>/cache/<name>` for each of its apps
and each image its builds target, unless an app or build of another live workspace of the space
uses the same name. Pulls of those repositories then answer 404. Blobs stay on the volume, since
other manifests may reference them and the registry has no collection yet; after
[lake#383](https://github.com/oramasearch/lake/issues/383) a workspace's images go with its store.

An app pull token is accepted only as HTTP Basic user `app`, with the token as its password. Lake
re-hashes it against the current app row and allows manifest and blob `GET`/`HEAD` only for the
exact `<space>/<app name>` repository. Other apps, cache repositories, upload starts, blob writes,
and manifest publication receive OCI `403 DENIED`. Rotation or app deletion revokes it.

## Protocol routes

| Protocol | Routes |
|---|---|
| OCI Distribution | `GET /v2/`; blob upload/read and manifest routes below `/v2/<repository>/...`, including nested `<space>/<name>` and `<space>/cache/<name>` repositories |
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
`RegistryProtocols.BuildPushCredentialIsLimitedToItsCustomerPrefix` proves Basic authentication,
private reads, and the cross-space `403 DENIED` boundary.
`RegistryProtocols.AppPullCredentialReadsOnlyItsOwnImage` publishes an image through the real
adapter, reads its manifest and blob with Basic user `app`, and refuses another app, the cache
repository, writes, a different Basic username, and bearer use.

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

# Published artifact indexing
# Published artifact indexing

Lake treats each retained build output as a published artifact. Its output path is the artifact
name and its immutable libliquid result digest is the version.

After repository sync publication, Lake invokes the libliquid format registry for every output and
records its members. `POST /v1/reindex_artifacts` accepts `{"id":"<repository-id>"}` to rebuild
those records for existing artifacts; it requires repository `write` access and returns counts for
visited artifacts, members, text members, binary members, and bounded decompositions.

Search uses `in:artifact:<output>[@<result>]`. See [search.md](search.md) for the query grammar,
limits, and member-result fields.
