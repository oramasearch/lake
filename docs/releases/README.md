# Releases

One page per tagged release, newest first. A page states what the release includes, what it
does not, what is knowingly still broken, and how it was verified.

- [v0.1.0](v0.1.0.md) — 2026-10-12. The first tagged release, so the page is the full inventory:
  every feature by area, all 554 deployed capabilities by name, every page of the web app and
  every guide. Note its known issues, above all the open `sign_in_sso` security defect. Image:
  `oramasearch/lake:v0.1.0` (the same digest as `20261012a`).

## Conventions

- **Cut a release from `main`**, which is what the deployment runs. The version is
  `version_string()` in `src/version.cpp` and `project(lake VERSION ...)` in `CMakeLists.txt`;
  move both in the pull request that adds the release page. Tests read the version from
  `lake::version_string()`. The steps are in the root `CLAUDE.md`, under *Cutting a release*.
- **Tag the merge commit** of that pull request `vX.Y.Z` and publish the GitHub release from the
  page.
- **A release is three tags with one name**: the Git tag, the GitHub release, and the same tag on
  the image in Docker Hub, naming the digest that is deployed. The cluster runs that image before
  the release is called done.
- **A first release, or one after a long gap, lists everything in it**, not a summary. Check the
  page against the deployed OpenAPI: every path is named on the page, and the page names no
  capability the deployment does not serve.
- **Every number is measured** and says what it was measured on. A figure with no run behind it
  does not go in.
- **Claims follow [`docs/design/status.md`](../design/status.md)**: implemented, exercised
  locally, deployed, and exercised in deployment are different things. A release page does not
  claim more than that page does.
- **Known issues are part of the release**, linked to their tracking issue. A security defect
  goes first.
- Cross-repo references are full URLs; issues in this repository are linked by number.
