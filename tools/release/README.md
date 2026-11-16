# Release tools

`publish-release-image.py <build-tag> <vX.Y.Z>` gives the deployed lake image its release tag in
Docker Hub (lake#994). It writes `oramasearch/lake:vX.Y.Z` only when every check passes, and pushes
nothing otherwise:

- the Git tag `vX.Y.Z` is on origin and on `origin/main`, and is the merge commit of the release pull
  request from `dev` into `main`, merged or approved by a login in [`approvers`](approvers);
- the build tag's image carries that commit as `org.opencontainers.image.revision`;
- `vX.Y.Z` does not exist in Docker Hub yet (a release tag is never moved or written twice);
- the repository answers an anonymous request with 404, before and after.

`--dry-run` runs every check and writes nothing. `approvers` lists who may approve a release
image; changing it is a pull request. `test_publish_release_image.py` checks each refusal with Git,
GitHub and the registry replaced (`python3 -I -m unittest discover -s tools/release -p 'test_*.py'`).

The steps, and the human step that turns on Docker Hub tag immutability, are in
[release-images.md](../../docs/deployment/release-images.md).
