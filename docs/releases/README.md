# Releases

One page per tagged release, newest first. A page states what the release includes, what it
does not, what is knowingly still broken, and how it was verified.

- [v0.8.1](v0.8.1.md) — 2026-10-27. A patch: a file's exact text, clustering on real columns,
  predicting on a clustering model, Change App reloading onto the new version, and end-to-end fixes.

Notes for earlier versions are not published.

## Conventions

- **Cut a release from `main`**, which is what the deployment runs. The version is
  `version_string()` in `src/version.cpp`, `current_version()` in `include/lake/version.hpp` and
  `project(lake VERSION ...)` in `CMakeLists.txt`; move all three in the pull request that adds
  the release page. `ReleaseVersion.*` fails when they disagree or the page is missing. Tests read the version from
  `lake::version_string()`.
- **Tag the merge commit** of that pull request `vX.Y.Z` and publish the GitHub release from the
  page.
- **A release is three tags with one name**: the Git tag, the GitHub release, and the same tag on
  the published image, naming its digest.
- **A first release, or one after a long gap, lists everything in it**, not a summary. Check the
  page against the deployed OpenAPI: every path is named on the page, and the page names no
  capability the deployment does not serve.
- **Every number is measured** and says what it was measured on. A figure with no run behind it
  does not go in.
- **Claims are exact**: implemented, exercised locally, deployed, and exercised in deployment are
  different things. A release page does not claim more than was done.
- **Known issues are part of the release**, linked to their tracking issue. A security defect
  goes first.
- **Every page is written for the public**: no deployment names, credentials, people or links to
  private trackers. Cross-repo references name the repository and number.
