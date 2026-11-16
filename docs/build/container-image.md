# Linux/amd64 container image

`Dockerfile` builds `lake` on Ubuntu 24.04 with clang-19 from the LLVM apt channel,
`libstdc++-14-dev`, checksum-verified OpenSSL 3.6.3, libpq development packages, `pkg-config` and
`libfuse3-dev` for libliquid discovery, Ninja, and CMake 3.31.5. It is Zetta's headless-image
recipe minus the GUI and Postgres dependencies. The runtime image is a minimal Ubuntu with the
matching FUSE, OpenSSL, C++ and libpq runtime libraries; it runs as numeric UID/GID `10001`, with
`ENTRYPOINT lake`, `CMD --listen 0.0.0.0:4900`, and port 4900 exposed.
The build also requires a named `zetta_source` context pointing at the authorized
`zetta-bridge/modules/zetta` checkout. The Dockerfile copies that context to `/opt/zetta` and passes
the same path as `LAKE_ZETTA_SOURCE_DIR`; Lake compiles the client transport and its runtime event
implementation.

The list of Zetta sources is `zetta-api` in `CMakeLists.txt`. Since Zetta v0.5.0 the native HTTP
transport enforces the egress policy (zetta#1321) and the destination policies (zetta#1452,
zetta#1469) and reads the system CA bundle itself, so the list also names
`src/core/connect/egress_policy.cpp`, `connector_destinations.cpp`, `destination_policy.cpp` and
`src/core/api/system_trust_store.cpp` (lake#1086). When a new Zetta pin fails to link with undefined
`zetta::` symbols, add the source that defines each one to that list. The minimal context the
cluster BuildKit lane uses (`build/contexts/zetta-pin`) is
`git archive <pin> include src` of the bridge's Zetta checkout, so it carries every one of them;
regenerate it whenever the pin moves.

## Publish a deployment image

Deployment images are built by GitHub Actions, not by a developer workstation, and go to the
**private** Docker Hub repository `oramasearch/lake`. After the workflow's credentials are
configured, create an immutable Docker-compatible Git tag and push it:

```sh
git tag <tag>
git push origin <tag>
```

The `image` workflow checks out the pinned bridge source,
supplies its Zetta module as the named build context, builds `linux/amd64`, and publishes
`oramasearch/lake:<tag>` plus `oramasearch/lake:sha-<short-sha>`. It rejects `latest`; `latest`
and workstation-built deployment images are not used. The workflow supplies the full checked-out
source revision and a UTC creation time as build arguments. The runtime image stamps them as
`org.opencontainers.image.revision` and `org.opencontainers.image.created`; the binary embeds the
revision and returns it in `GET /v1/server_info`.

Wait for the workflow to succeed before updating `Lake.spec.image`.

## Run it under the operator

The `Lake` CR on `orama-operator` runs the image: `spec.image: oramasearch/lake:<tag>`,
`spec.pullSecretName: <registry-secret>` (the Secret must exist in `spec.namespace`), port
4900 by default (`spec.port`), and no args needed.

## Local verification

Identity and API changes must first prove that the production Linux image compiles:

```sh
docker build --platform linux/amd64 \
  --build-context zetta_source=<zetta-bridge>/modules/zetta .
```

For local image inspection, build and load a named image:

```sh
docker buildx build --platform linux/amd64 \
  --build-context zetta_source=<zetta-bridge>/modules/zetta \
  -t oramasearch/lake:<local-tag> --load .
docker run --rm -p 4900:4900 oramasearch/lake:<local-tag> version
```

Do not deploy or push a workstation-built image. A CI-built immutable tag is the deployment
artifact. On Apple Silicon the local `linux/amd64` build uses emulation; CI and the AKS builder
produce the release image natively.

## Change the image recipe

Keep `Dockerfile` and `.dockerignore` as the image recipe; the context excludes `build/` and
`.git/`. When the build-stage toolchain changes, update the CI workflow and
[`local.md`](local.md) in the same change so pull-request builds and local
builds use the image toolchain.

### Pinned inputs (lake#1003)

Every base image is named by tag and digest, so a rebuild of the same source gets the same bytes
even when the tag moves: `node:22-bookworm-slim@sha256:e9f565d3…` for the web app stage and
`ubuntu:24.04@sha256:2198e4c6…` for the build and runtime stages (the image index digest, which
covers every platform). The CMake installer is checked against the SHA-256 Kitware publishes
(`cmake-<version>-SHA-256.txt`) before it runs, as OpenSSL's archive already was. In
`CMakeLists.txt` the Boost archive carries the `URL_HASH` Boost publishes beside it
(`<archive>.txt`), and GoogleTest is fetched by the commit of its `v1.17.0` tag (`52eb8108…`), not
by the tag. A changed archive fails configure.

To move one, read the new value without pulling or pushing anything and change it in a reviewed
commit:

```sh
docker buildx imagetools inspect ubuntu:24.04 | grep '^Digest:'
curl -sL https://github.com/boostorg/boost/releases/download/<release>/<archive>.txt
curl -sL https://github.com/Kitware/CMake/releases/download/v<version>/cmake-<version>-SHA-256.txt
git ls-remote https://github.com/google/googletest.git 'refs/tags/<tag>*'   # the ^{} line for an annotated tag
```

Not pinned by this change: the LLVM apt script (`llvm.sh` installs packages signed by a key fetched
from the same host, so a hash of the script alone would not help), and libliquid's own
`FetchContent` dependencies, which liquid's `CMakeLists.txt` selects, not lake.

## The compiler cache

The build stage compiles through `ccache` into a BuildKit cache mount (`id=lake-ccache`, 8 GiB),
which outlives each image. The source always sits at `/src`, so a later build recompiles only the
sources that changed rather than all of lake, libliquid and the vendored stack. The build revision
is defined for `src/version.cpp` alone, so a new revision invalidates one object. `ccache
--show-stats` at the end of the build step shows the hit rate. To start cold, run `buildctl prune --all`
against the BuildKit pod; it clears every cache, this one included.

The tunnel client binaries Developer Access hands out (lake#745) come from the private scratch image
`oramasearch/zetta-tunnel-clients`, named in a `FROM ... AS tunnel-clients` stage by tag and digest;
the runtime stage copies its `/clients/` to `/usr/share/lake/tunnel-clients/`
([tunnel-clients.md](../api/tunnel-clients.md)). BuildKit pulls it with the same registry credential
as the base images (`DOCKER_CONFIG` in `build-lake.sh`). A new client version is a new digest here.

The web app is built in its own Node 22 stage (`frontend`) and embedded with
`-DLAKE_FRONTEND_DIST`; see [docs/ui/web-app.md](../ui/web-app.md). The stage copies `frontend/` and also
`assets/catalogue-icons/`, because the frontend's test suite (run by `npm run build`) checks that the
exported icon copies match the source (lake#639), and `src/api/engine_openapi.json`, because the agents
test checks the vendored skills against the engine's `create_agent` schema (lake#653). A test that reads
another file outside `frontend/` needs it copied here too.
