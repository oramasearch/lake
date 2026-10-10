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

## Publish a deployment image

Deployment images are built by GitHub Actions, not by a developer workstation, and go to the
**private** Docker Hub repository `oramasearch/lake`. After the required credentials in
[`ci.md`](ci.md) are configured, create an immutable Docker-compatible Git tag and push it:

```sh
git tag <tag>
git push origin <tag>
```

The [`image` workflow](../../.github/workflows/image.yml) checks out the pinned bridge source,
supplies its Zetta module as the named build context, builds `linux/amd64`, and publishes
`oramasearch/lake:<tag>` plus `oramasearch/lake:sha-<short-sha>`. It rejects `latest`; `latest`
and workstation-built deployment images are not used. The workflow supplies the full checked-out
source revision and a UTC creation time as build arguments. The runtime image stamps them as
`org.opencontainers.image.revision` and `org.opencontainers.image.created`; the binary embeds the
revision and returns it in `GET /v1/server_info`.

Wait for the workflow to succeed before updating `Lake.spec.image`; the deployment runbook is in
[`../deployment/aks.md`](../deployment/aks.md).

## Run it under the operator

The `Lake` CR on `orama-operator` runs the image: `spec.image: oramasearch/lake:<tag>`,
`spec.pullSecretName: oramasearch-dockerhub` (the Secret must exist in `spec.namespace`), port
4900 by default (`spec.port`), and no args needed. The manual dispatch path for an unpublished tag
is in [`ci.md`](ci.md).

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
`.git/`. When the build-stage toolchain changes, update [`ci.yml`](../../.github/workflows/ci.yml),
[`ci.md`](ci.md) and [`local.md`](local.md) in the same change so pull-request builds and local
builds use the image toolchain.

## The compiler cache

The build stage compiles through `ccache` into a BuildKit cache mount (`id=lake-ccache`, 8 GiB),
which outlives each image. The source always sits at `/src`, so a later build recompiles only the
sources that changed rather than all of lake, libliquid and the vendored stack. The build revision
is defined for `src/version.cpp` alone, so a new revision invalidates one object. `ccache
--show-stats` at the end of the build step shows the hit rate. To start cold, prune the cache on
the BuildKit pod (`buildctl prune --filter id=lake-ccache` is not selective; `buildctl prune
--all` clears every cache).

The web app is built in its own Node 22 stage (`frontend`) and embedded with
`-DLAKE_FRONTEND_DIST`; see [docs/ui/web-app.md](../ui/web-app.md).
