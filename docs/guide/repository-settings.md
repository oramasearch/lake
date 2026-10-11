# Branch Protection, Webhooks, Import and Export

The settings that govern a repository beyond its files: which pushes a reference accepts, which
outside systems hear about its events, and how a repository moves in and out of lake with its
history, reviews and grants intact.

These are capabilities first. The web app shows webhooks and their deliveries (with **Retry**),
shows branch protection rules to the instance operator, and downloads an export; rules, webhooks
and imports are created through the API or MCP.

## Concepts

| Term | What it is |
|---|---|
| **Branch protection rule** | A rule on one reference (`main`) or a prefix ending in `*` (`release/*`) that every push to it must pass. |
| **Refused update** | A push to a protected reference that a rule turned away. The push's reply names the reference and the rule's reason, and lake records a signed audit event. |
| **Webhook** | A URL lake posts a repository's issue, review and release events to, signed with a secret you choose. Every attempt is kept. |
| **Export archive** | A signed `lake_export/1` JSON file holding a repository's history and content, issues, reviews, releases, webhooks, direct grants and build evidence, and optionally a Git copy. |
| **Import** | Recreating a repository from an export archive, or from a Git checkout plus recorded GitHub data, as a new repository in a workspace you name. |

## Branch Protection

A rule may require any combination of:

| Field | Effect |
|---|---|
| `required_review` | The change being pushed must have a review in state `approved` that names exactly that change. |
| `required_verification` | The change must have verified release evidence. |
| `fast_forward_only` | The new head must contain the old one; rewriting history is refused. |
| `allowed_pushers` | Only these TAAP identities may update the reference. |

A pusher also needs `write` on the reference itself, so repository grants still apply and a grant
on the reference can narrow them.

### Set a Rule

- REST: `POST /v1/set_branch_protection` with
  `{"repository_id": "<repository id>", "ref": "main", "required_review": true, "fast_forward_only": true}`.
  Setting a rule for a `ref` that already has one updates it.
- MCP: `set_branch_protection` with the same input.

### See the Rules and Refused Updates

- `GET /v1/list_branch_protections?repository_id=<repository id>` (MCP `list_branch_protections`)
  lists the rules.
- The repository's **Protection** tab, shown to the instance operator, lists each rule and the
  **Refused Updates**: reference, caller, matching rule, reason and date.
- The pusher sees the refusal in the push itself: liquid reports each reference as applied or
  refused, with the rule's reason, for example *branch protection rejects a non-fast-forward update
  to main*.

### How a Push Is Judged

- Each reference in a push gets its own verdict. An ordinary push applies the accepted references
  and leaves refused ones where they were.
- `liquid push --atomic` asks for all or nothing: one refusal leaves every reference unchanged, and
  the reply still lists every outcome.
- Lake reads the rules from the repository's workspace before the push. If it cannot (the
  workspace is not ready), every reference update in that push is refused with that reason, rather
  than let an unchecked push through.

## Webhooks

Lake posts an event when an issue or issue comment is created (`issue`), when a review or review
comment is created (`review`), and when a release is created (`release`). A webhook subscribed to
`*` receives all three.

### Add a Webhook

- REST: `POST /v1/create_webhook` with
  `{"repository_id": "<repository id>", "url": "https://hooks.example.com/lake", "events": ["issue", "release"], "secret": "<a long random string>"}`.
- MCP: `create_webhook` with the same input.

The secret is stored and never shown again; `list_webhooks` omits it.

### What Your Endpoint Receives

Each delivery is a `POST` with a JSON body naming the `event`, the `repository_id` and the
`record` (the issue, review or release), and these headers:

| Header | Value |
|---|---|
| `content-type` | `application/json` |
| `x-lake-event` | the event: `issue`, `review` or `release` |
| `x-lake-signature-256` | `sha256=` followed by the HMAC-SHA-256 of the exact body under your secret, in unpadded base64url |

Verify the signature over the raw body before trusting it. Any 2xx answer counts as delivered.

### Follow and Retry Deliveries

1. Open the repository's **Webhooks** tab. Figures count **Webhooks**, **Deliveries** and
   **Failed**; each webhook lists its attempts with event, attempt number, status and error.
2. On a failed attempt press **Retry**. A retry is a new attempt; earlier attempts stay listed.

- List: `GET /v1/list_webhooks?repository_id=<repository id>` and
  `GET /v1/list_webhook_deliveries?repository_id=<repository id>` (MCP `list_webhooks`,
  `list_webhook_deliveries`).
- Retry: `POST /v1/retry_webhook_delivery` with
  `{"repository_id": "<repository id>", "delivery_id": "<delivery id>"}` (MCP
  `retry_webhook_delivery`). Only an unsuccessful attempt can be retried.
- Send a test event: `POST /v1/deliver_webhook` with
  `{"repository_id": "<repository id>", "webhook_id": "<webhook id>", "event": "issue", "payload": {"test": true}}`
  (MCP `deliver_webhook`). The attempt is recorded like any other.

## Export a Repository

An export is a portable, signed copy: the history and every object, the issues, reviews, comments,
releases and webhooks (without secrets), direct grants and build evidence. Each part has a SHA-256
digest in a manifest, and the manifest is signed by this lake's identity, so the destination can
check it without reaching back here.

1. Open `/exports?repository_id=<repository id>` (the **Repository Export** page).
2. Tick **Include Git projection** if you also want a plain Git copy for Git tools.
3. Press **Request and download export**. The archive downloads as `lake-export.json`. **Prior
   Exports** lists earlier ones.

- REST: `POST /v1/export_repository` with
  `{"repository_id": "<repository id>", "include_git": true}`. The answer's `archive` is the file
  to keep.
- MCP: `export_repository` with the same input.
- Earlier exports: `GET /v1/list_exports?repository_id=<repository id>` (MCP `list_exports`).

Exporting needs only `read` on the repository.

## Import an Export Archive

`POST /v1/import_repository` (MCP `import_repository`) creates a new repository from an archive.
Send `customer_id`, `workspace_id` (the workspace the copy belongs to), the new `name`, and
`archive`: the archive object exactly as `export_repository` answered it, or as the downloaded file
contains it.

Lake verifies every part's digest and the signature before it creates anything, so a changed or
damaged archive is refused. The new repository keeps the source's description and visibility,
gets new ids for its issues, reviews and webhook deliveries, and recreates the direct grants on
itself. It never reuses the source's workspace: you name where the copy goes.

This is also how a repository moves to another owner or workspace, since a repository never
changes its own.

## Import from GitHub

`POST /v1/import_github_repository` (MCP `import_github_repository`) imports a Git history together
with GitHub's issues, comments, pull requests, review comments and workflow files. Send
`customer_id`, `workspace_id`, the new `name`, `git_directory` (a Git checkout the lake server can
read) and `recording`: the GitHub REST responses for `repository`, `issues`, `issue_comments`,
`pulls`, `review_comments` and `workflows`, collected beforehand. Lake makes no call to GitHub
itself, and no GitHub credential is stored.

| From GitHub | Becomes |
|---|---|
| Git history | liquid changes and references, lossless |
| Issues and comments | issues and issue comments, with state, labels, assignees and author |
| Pull requests and review comments | reviews and anchored review comments, when their commit is in the imported history |
| Actions workflows | recorded documents with the build commands `liquid wrap` replaces; never run |
| Reactions, checks, Actions runs | not imported; listed as a limit |

The answer includes a fidelity report (`github_import_fidelity/1`) of what was mapped and what was
not, and the report is kept with the repository.

## Limits

- A JSON request body is at most 64 MiB, which bounds the size of an archive sent to
  `import_repository`.
- Webhooks fire on creation of issues, issue comments, reviews, review comments and releases; state
  changes do not send an event.
- Only the instance operator sees the **Protection** tab; everyone reads rules with
  `list_branch_protections`.
- A pull request whose head commit is missing from the imported Git history stays unmapped.

## Permissions

| To | You need |
|---|---|
| Set branch protection, add webhooks, retry or send deliveries | `write` on the repository |
| Read rules, webhooks and deliveries | `read` on the repository, signed in |
| Export | `read` on the repository |
| Import (archive or GitHub) | `write` on the account that owns the destination workspace |

## Troubleshooting

| You see | Why, and what to do |
|---|---|
| A push says a reference was refused | A rule turned it away; the reason names the rule. Get the change approved, push a fast-forward, or ask to be added to `allowed_pushers`. |
| Every reference refused with `branch_protection_unavailable` | The repository's workspace could not be read before the push. Wait until it is ready and push again. |
| Deliveries fail with *webhook sender is not configured* | This lake cannot send outbound webhooks. Attempts are kept and can be retried once it can. |
| `invalid_digest` or `invalid_signature` on import | The archive was changed or damaged after export. Export it again. |
| `conflict` at `/name` on import | A live repository already has that name. Choose another. |
| `invalid_input` at `/workspace_id` | The destination workspace is missing, unknown or deleted. |
| `conflict` at `/customer_id` on `transfer_repository` | Repositories never change owner. Export and import instead. |

## Related Pages

- [Repositories](/docs/repositories)
- [Reviews, Issues and Releases](/docs/repository-collaboration)
- [The liquid CLI](/docs/cli)
- [Activity and Audit](/docs/activity)
- [Grants and Effective Access](/docs/access)
