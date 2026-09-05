# GitHub importer

**What.** `import_github_repository` imports a Git checkout through liquid's `import_git` API and
maps a recorded GitHub REST-response bundle into lake issues, discussion, reviews, review anchors,
workflow documents, and a durable `github_import` fidelity report.

**Why.** Git commits transfer cleanly; GitHub's issue, review, and Actions data has no portable
Git representation. Recording the exact mapping and omissions makes a migration reviewable rather
than implying lossless conversion.

## Input

The action accepts destination `customer_id` and `name`, a local `git_directory`, and a
`recording` object. The recording has `repository`, `issues`, `issue_comments`, `pulls`,
`review_comments`, and `workflows` arrays copied from GitHub API responses. Tests use the
checked-in `tests/fixtures/github-import-recording.json` response bundle and no network connection.
A deployment integration supplies the recorded
responses before calling this capability; credentials never enter a forge record.

Liquid imports the Git history before any forge fact. Its `git.commit.<sha>` mappings bind each
pull-request head and review-comment commit to a liquid change identity. A pull request whose head
is absent from the imported Git graph remains in the report's `not_mapped` list rather than gaining
an invented change target.

## Fidelity report

`github_import_fidelity/1` records imported commit/blob counts, mapped fact categories, facts that
could not be mapped, workflow documents, and the present limit: GitHub reactions, checks, and
Actions execution history are not imported. A workflow document retains its source path and
content plus every CMake, Make, or Autotools command for which `liquid wrap` is the replacement.
The report is stored as a `github_import` forge record and returned with the destination repository.

| GitHub value | Lake value | Fidelity |
|---|---|---|
| Reachable Git history | liquid changes and refs | lossless through `import_git` |
| Issue and comment | issue and issue-comment forge records | body, state, labels, assignees, author retained |
| Pull request and review comment | review and anchored review-comment records | retained when the Git SHA maps to a liquid change |
| Actions workflow | recorded workflow document and wrap report | analysed, never executed or imported as CI |
| Reactions, checks, Actions runs | fidelity-report limit | not mapped |

## Verify

```sh
cmake --build build
ctest --test-dir build -R GitHubImport --output-on-failure
```

The recorded fixture maps one issue, discussion comment, pull request, anchored review comment,
and workflow build command. The test asserts the returned fidelity format, `liquid wrap` report,
and created forge records without contacting GitHub.

## Change

Add a response field only with a documented source-to-lake mapping or a fidelity-report limit.
Never make an HTTP call inside the capability handler: the recording is the capability boundary and
keeps replay tests deterministic.
