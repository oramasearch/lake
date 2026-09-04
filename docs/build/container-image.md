# Linux/amd64 container image

`Dockerfile` builds `lake` on Ubuntu 24.04 with clang-19 from the LLVM apt channel,
`libstdc++-14-dev`, OpenSSL and libpq development packages, Ninja, and CMake 3.31.5. The runtime
image contains only the runtime libraries and runs as numeric UID/GID `10001`.

## Publish a deployment image

Deployment images are built by GitHub Actions, not by a developer workstation. After the required
credentials in [`ci.md`](ci.md) are configured, create an immutable Docker-compatible Git tag and
push it:

```sh
git tag <tag>
git push origin <tag>
```

The [`image` workflow](../../.github/workflows/image.yml) builds `linux/amd64` and publishes
`oramasearch/lake:<tag>` plus `oramasearch/lake:sha-<short-sha>`. It rejects `latest`.

Wait for the workflow to succeed before updating `Lake.spec.image`; the deployment runbook is in
[`../deployment/aks.md`](../deployment/aks.md).

## Local verification

For local image inspection, build natively on amd64:

```sh
docker buildx build --platform linux/amd64 -t oramasearch/lake:<local-tag> --load .
docker run --rm -p 4900:4900 oramasearch/lake:<local-tag> version
```

Do not deploy or push a workstation-built image. A CI-built immutable tag is the deployment
artifact.

## Change the image recipe

Keep `Dockerfile` and `.dockerignore` as the image recipe. When the build-stage toolchain changes,
update [`ci.yml`](../../.github/workflows/ci.yml) and [`ci.md`](ci.md) in the same change so
pull-request builds use the image toolchain.
