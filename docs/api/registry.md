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

Since lake#383 phase A, `manifests/` holds only the instance's one-component OCI repositories, and
what it still holds of tenant repositories waits for the one-shot copy and is not served (see
[below](#where-a-tenant-image-lives)). `blobs/` holds every blob, tenant blobs included, until
phase B.

Writes use a sibling temporary file followed by rename. OCI upload sessions are staged separately;
completion recalculates SHA-256 and refuses bytes that do not match the requested digest. A
manifest is accepted only when it is schema version 2 JSON and every referenced config or layer
digest is already present. Blob and digest-manifest reads also require that a tag in the requested
repository references the object, so a known tenant digest cannot be read through a public alias.

## Where a tenant image lives

The registry splits (decision 54 S4-S6, [lake#383](https://github.com/oramasearch/lake/issues/383)
phase A). A tenant OCI repository, `<space>/<name>` or `<space>/cache/<name>`, belongs to one
workspace. npm, Python, debuginfod and legacy one-component OCI repositories stay with the instance,
as a named exception: one instance-wide namespace with public reads.

- **The route.** `oci_repository_routes` (migration `0068`) maps the space and the name below it
  (`<image>` or `cache/<image>`; no other shape is a name) to a workspace. It is content-free: no
  manifest, digest or tag. Lake resolves it before it reads any workspace, so an image keeps its
  address (`<in-cluster-registry-host>/<space>/<app>` for the kubelet). A route is never changed
  in place; it is deleted with its workspace, or by the operator (below).
- **Creating a repository names its workspace.** A build's first push routes only its own target,
  `<image>` and `cache/<image>` of `<authority>/<space>/<image>:<tag>`, from the build row's
  workspace; a build pushing any other unrouted name of its space is OCI `403 DENIED`. Anything
  else (the administrator, a person) calls `create_image_repository` with `customer_id`,
  `workspace_id` and `name` first; a missing, malformed or unknown `workspace_id`, or one that is
  not a live workspace of the space, is `invalid_input` at `/workspace_id`, and an invalid name at
  `/name`. There is no default workspace. Repeating it for the same workspace answers the route; a
  name another workspace holds is `conflict`. A push to an unrouted repository is OCI
  `404 NAME_UNKNOWN`.
- **Removing or moving a wrong route.** `unroute_image_repository` (`customer_id`, `name`,
  optional `workspace_id`; operator bearer only, audited in the central log as control plane)
  deletes a route, for example after an app was recreated in another workspace. Removing `<image>`
  also removes `cache/<image>`; removing `cache/<image>` alone leaves `<image>`. With
  `workspace_id` it moves them there instead: the routes are deleted and written again in one
  transaction, so a build of the old workspace still pushing cannot claim the name in between. The
  target must be a live workspace of the space (otherwise `invalid_input` at `/workspace_id`, and
  nothing changes). The old workspace's manifest and tag records stay where they are and go with
  that workspace; after a plain removal a later `create_image_repository`, build push or copy
  routes the name again.
- **Manifests and tags are workspace records** (sealed, as every lake record,
  [workspace records](../architecture/workspace-records.md)). `lake_oci_manifests` holds one record
  per manifest of the space, keyed by the blind index of its digest, written `if_absent`: the exact
  bytes, the media type and the digests it references with their sizes. `lake_oci_tags` holds one
  record per tag, keyed by the blind index of the repository and tag and found by the repository,
  so one `list_records` reads every tag of a repository. A push moves a tag with a write
  conditional on the revision read (`if_absent` for a new tag), rereading on a conflict: a tag is
  never torn and every move is applied whole, but two concurrent pushes of one tag still end with
  the last one (last writer wins, as OCI expects). A tag carries its manifest's references, so a
  blob read and the image byte count read only the tags; a blob `GET` lists every tag of its
  repository.
- **Blobs stay in the instance registry** (`blobs/`) until phase B
  ([lake#380](https://github.com/oramasearch/lake/issues/380)) moves them into the workspace's
  liquid store. Each blob an upload into one of a workspace's repositories completes is recorded in
  `lake_oci_blobs` of that workspace, and a manifest pushed into the workspace may reference only
  blobs recorded there (otherwise OCI `400 MANIFEST_BLOB_UNKNOWN`), so knowing another space's or
  another workspace's digest does not publish its layer. A tenant blob is served only when a tag
  of the requested repository references it.
- **Workload credentials stay in their workspace.** A build pushes and reads only repositories
  routed to its own workspace, so a build cannot pull a base or cache image of another workspace
  of its space; an app pull token reads only its app's repository, only when the route names the
  app's workspace, and never for an app without a workspace. Another workspace's route is OCI
  `403 DENIED`.
- **Routes or a workspace that cannot be read** are OCI `503 UNAVAILABLE`; lake never answers
  "unknown" for them and never falls back to another copy.
- `list_image_repositories` (`customer_id`, optional `workspace_id`; a workspace that is not the
  space's is `invalid_input`) lists a space's routes with the tags read from each workspace; a
  workspace that cannot be read lists its repositories with `tags_unavailable`. Ship → Packages
  shows the tenant images of one space, the selected one, from it.
- Usage counts a space's image bytes from the tags in each routed workspace, each digest once
  ([metering.md](metering.md)).

Tags written before the split stay in `manifests/` until the operator's one-shot copy
(`copy_workspace_records`, collection `oci_tags`) moves each into its workspace. Each run finds the
owner of each repository (its route; else the one live workspace whose apps or builds use the
image's name, `cache/<image>` following `<image>`; else the space's only live workspace); `plan`
and `verify` write nothing, and only `copy` writes the route of an unrouted repository. It reports
the rest as unresolved (`needs_selection`, resolved with `create_image_repository`;
`blob_missing`, a tag whose manifest or blobs the registry no longer holds; `unknown_space`,
which does not block completion), records the manifest's blobs, writes the manifest and the tag
`if_absent` (marked as copied), and verifies them. A tag a push wrote before the copy reached it is
newer and is kept and accepted. Until a tag is copied, its repository answers `404`. The cutover
steps are in
[workspace records](../architecture/workspace-records.md#cutover-runbook-tenant-images-dev-cluster).

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
the cascade transaction deletes its routes, and their manifest and tag records go with its Zetta
(lake#383). Whatever the instance registry still holds for the workspace from before the split
(the tags of `<space>/<name>` and `<space>/cache/<name>` for each of its apps and each image its
builds target, unless another live workspace of the space uses the name) is removed too. Pulls of
those repositories then answer 404. Blobs stay on the volume until phase B, since other manifests
may reference them and the registry has no collection yet.

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
`Stores/TenantImages.LiveInTheirRoutedWorkspaceAndKeepTheirAddresses/{Memory,Postgres}` runs two
spaces with two workspaces each: missing, malformed, unknown, foreign and deleted `workspace_id` and
a bad name refused, an unrouted push refused, a route repeated, never changed and shared by name with
another space, push and pull through the route byte for byte (by tag, by digest, blobs), sealed
records only in the routed workspace, a tag moved by a second push, a build confined to its own
workspace and routing on its first push, npm and one-component OCI unaffected, a workspace that is
not ready refused, and workspace deletion removing the route.
`Stores/TenantImages.TheOneShotCopyRoutesAndMovesLegacyTags/{Memory,Postgres}` covers the copy
(no route written by plan or verify, a tag pushed before the copy accepted, a lost blob reported),
`Stores/TenantImages.RoutesAreClaimedOnlyByTheirOwnerAndUnroutedOnlyByTheOperator/{Memory,Postgres}`
a build claiming only its own target, an app moved between workspaces and its route removed by
the operator, foreign layers refused, an app without a workspace, and unreadable routes as `503`,
`Stores/TenantImages.AMoveReroutesAnImageAndItsCacheInOneStep/{Memory,Postgres}` a move of an image
and its cache while the old workspace's build keeps pushing, and
`PostgresImageRoutes.MigrationRoutesUnambiguousImagesOnly` migration `0068`.

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
./build/tests/lake-tests --gtest_filter='RegistryProtocols.*:Stores/TenantImages.*'
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
