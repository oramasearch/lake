# Linux/amd64 container image

`Dockerfile` builds `lake` on Ubuntu 24.04 with clang-19 from the LLVM apt channel,
`libstdc++-14-dev`, checksum-verified OpenSSL 3.6.3, libpq development packages, `pkg-config` and
`libfuse3-dev` for libliquid discovery, Ninja, and CMake 3.31.5. The runtime image contains the
matching FUSE, OpenSSL, C++ and libpq runtime libraries and runs as numeric UID/GID `10001`.
The build also requires a named `zetta_source` context pointing at the authorized
`zetta-bridge/modules/zetta` checkout. The Dockerfile copies that context to `/opt/zetta` and passes
the same path as `LAKE_ZETTA_SOURCE_DIR`; Lake compiles only the client transport sources.

## Publish a deployment image

Deployment images are built by GitHub Actions, not by a developer workstation. After the required
credentials in [`ci.md`](ci.md) are configured, create an immutable Docker-compatible Git tag and
push it:

```sh
git tag <tag>
git push origin <tag>
```

The [`image` workflow](../../.github/workflows/image.yml) checks out the pinned bridge source,
supplies its Zetta module as the named build context, builds `linux/amd64`, and publishes
`oramasearch/lake:<tag>` plus `oramasearch/lake:sha-<short-sha>`. It rejects `latest`.

Wait for the workflow to succeed before updating `Lake.spec.image`; the deployment runbook is in
[`../deployment/aks.md`](../deployment/aks.md).

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

Keep `Dockerfile` and `.dockerignore` as the image recipe. When the build-stage toolchain changes,
update [`ci.yml`](../../.github/workflows/ci.yml) and [`ci.md`](ci.md) in the same change so
pull-request builds use the image toolchain.
