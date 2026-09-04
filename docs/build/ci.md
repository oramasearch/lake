# Continuous integration and image publishing

GitHub Actions has two workflows that keep the repository buildable and publish deployment
images. The Dockerfile remains the canonical Linux toolchain recipe; CI installs the same Ubuntu
24.04 packages, LLVM 19 channel and CMake 3.31.5 installer.

## Build and test workflow

[`ci.yml`](../../.github/workflows/ci.yml) runs for every push and pull request. It checks out all
submodules recursively, configures Release with `LAKE_BUILD_TESTS=ON`, builds with clang-19, then
runs:

```sh
ctest --test-dir build --output-on-failure
```

The workflow caches `build/_deps`, which contains the CMake FetchContent downloads for Boost,
nlohmann/json and GoogleTest. Its key includes the hash of `CMakeLists.txt`; a missing cache is
valid and CMake downloads the dependencies again.

## Tagged image workflow

[`image.yml`](../../.github/workflows/image.yml) runs when any Git tag is pushed. It builds only
`linux/amd64` and pushes these Docker Hub tags:

```text
oramasearch/lake:<git-tag>
oramasearch/lake:sha-<short-sha>
```

`latest` is rejected. Git tags used for images must also be valid Docker tag strings. Deployments
reference the Git-tag image; the SHA tag provides a second immutable source identifier.

The Linux/amd64 image is verified by this native CI worker or the AKS builder after
`ORAMA_SUBMODULES_TOKEN` is available. Apple Silicon developer machines do not need to emulate the
amd64 image recipe; that emulation is not a release proof.

The workflow can also be dispatched manually with the required `tag` input. Use that path only to
retry an unpublished tag, selecting the matching Git tag as the workflow ref:

```sh
gh workflow run image.yml --repo oramasearch/lake --ref <git-tag> -f tag=<git-tag>
```

Do not dispatch an already-published tag. Create and push a new Git tag for a new image.

## Required credentials

Steve must create these Actions secrets before the workflows can complete their external work:

| Secret | Scope and use |
|---|---|
| `ORAMA_SUBMODULES_TOKEN` | Organization-level Actions secret available to `oramasearch/lake`; a fine-grained token with read access to the organization's private repositories, passed to recursive `actions/checkout`. |
| `DOCKERHUB_USERNAME` | Repository Actions secret containing the Docker Hub account allowed to push `oramasearch/lake`. |
| `DOCKERHUB_TOKEN` | Repository Actions secret containing that account's Docker Hub access token with push permission. |

Until `ORAMA_SUBMODULES_TOKEN` exists, CI stops during checkout. Until the two Docker Hub secrets
exist, a tag build cannot log in or publish. Neither secret is created by this repository or its
workflows.

## Verify and change

Validate workflow syntax locally with `actionlint .github/workflows/*.yml`. After merging, a push
or pull request must show the `ci` check passing. Push a new Git tag and inspect the `image`
workflow before changing a `Lake` resource to that tag.

When the Dockerfile toolchain changes, make the corresponding package, LLVM and CMake changes in
`ci.yml` in the same change. Keep the image workflow restricted to `linux/amd64` and immutable
tags.
