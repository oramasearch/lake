# Release images: a release tag is written once, from an approved source

> **Human action: open.** Turn on tag immutability for `v*` tags of `oramasearch/lake` in
> Docker Hub (below). Until it is on, `tools/release/publish-release-image.py` is the only guard
> against a release tag being overwritten. The hosted `.github/workflows/image.yml` is commented out
> (lake#994).

lake#994: a release tag such as `oramasearch/lake:v0.8.1` could be written again, and
nothing tied the bytes under it to an approved source. The fix:
the path that actually publishes images (the cluster BuildKit build and the registry retag) is
hardened; `image.yml` is commented out.

## The path

1. **Build** on the cluster BuildKit from the release tag's commit, under a **build tag**
   (`<YYYYMMDD><letter>`), with `LAKE_BUILD_REVISION=$(git rev-parse HEAD)`. The Dockerfile writes
   it into the label `org.opencontainers.image.revision`. A build never pushes a `vX.Y.Z` name. The
   commands are in liquid-bridge's `liquid-dev` skill.
2. **Roll and check** as in [CLAUDE.md, *Cutting a release*](../../CLAUDE.md#cutting-a-release).
3. **Name the release**, without rebuilding:
   ```sh
   export DOCKER_CONFIG=~/dev/oramasearch/codex-work/build/.dockercfg
   python3 -I tools/release/publish-release-image.py <build-tag> vX.Y.Z --dry-run
   python3 -I tools/release/publish-release-image.py <build-tag> vX.Y.Z
   ```
   Run it from a lake checkout whose `origin` is `oramasearch/lake`, with `gh` signed in. It refuses,
   with nothing written, when:
   - the version is not `vX.Y.Z`, or the build tag is a release name, `latest`, `main` or `dev`;
   - the Git tag is not on origin, is not on `origin/main`, or is not the merge commit of a merged
     release pull request from `dev` into `main` merged or approved by a login in
     `tools/release/approvers`;
   - the build tag's image was built from another commit (its revision label);
   - `vX.Y.Z` already exists in Docker Hub, whatever digest it names;
   - the repository answers an anonymous request (it must stay private).

   It reads the existing tags with a pull-only token, and requests a push token only after every
   check passed. It reads the new tag back and fails if it does not name the build tag's digest.
4. The deployed tag stays the build tag; `vX.Y.Z` is a second name for the same digest.

`retag-image.py` in the `liquid-dev` skill already refuses to move a tag that names a different
digest; for lake, use this script instead, which also checks the source.

## Human step: tag immutability in Docker Hub

**Who:** a Docker Hub organization owner. **Status:** open.

- **Symptom:** without it, anyone holding the push credential can write `vX.Y.Z` again by hand.
- **Read-only check:** in Docker Hub, `oramasearch/lake` → *Settings* → *Tag immutability*: it is
  off, or does not cover `v*`.
- **Steps:** turn tag immutability on for the pattern `v*` (release tags only: build tags such as
  `dev-<sha>` stay writable for the dev roll). Leave the repository private.
- **Confirm:** `publish-release-image.py` for a version that already exists refuses before pushing,
  and a manual `PUT` of an existing `v*` tag through the registry API answers an error.

## Deploying by digest

Production should pin `oramasearch/lake@sha256:…` next to its tag in the `lake-prod` set file. That
file is a cluster resource (moving to `orama-infra`), applied with consent; it is not changed here.
