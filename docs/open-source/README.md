# Open-Source Release

lake is published under the MIT licence as part of the Orama Lake open-source release. Before it
is published, the tree must pass five gates: security, secrets, internal material, licences, and
usable from public sources alone. This page records what each gate needs, what covers it, and how
to check it. The tracking issue is lake#1275.

## Gates

| Gate | What it needs | Covered by | Check |
|---|---|---|---|
| G1 Security | No open `security` issue; a private way to report one; a final review of the commit to be published | [`SECURITY.md`](../../SECURITY.md) (lake#1290); the open `security` issues and their pull requests | `gh issue list --label security --state open`; the repository's *Private vulnerability reporting* setting is on |
| G2 Secrets | A second secret scanner over the tree and the full history, every finding triaged, every credential that ever appeared rotated; test fixtures and smoke tokens confirmed not live | [The secret scan](#the-secret-scan) below: two scanners over the full history, every finding a fixture | A person confirms the triage; the rotation inventory |
| G3 Internal material | No internal names, hosts, paths, agent instruction files, private-repository references, quoted decisions or internal pricing in the tree | lake#1320, lake#1317, lake#1322 | [`tools/release/public-scan.sh`](#the-scan) comes back empty |
| G4 Licences | `LICENSE` (MIT), `COPYRIGHT`, an SPDX header in every source file we wrote, a generated `THIRD_PARTY_LICENSES` with nothing incompatible | lake#1321, lake#1334 | Every file in the tree we wrote carries `SPDX-License-Identifier: MIT` |
| G5 Usable | Builds and passes its tests from public sources on a clean machine; README, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`; a release with notes; a public, signed image with an SBOM; no default pointing at an Orama endpoint; native sign-in, billing off and a local model as the self-host defaults; no hosted CI | lake#1321 (contributor docs), lake#1307 (sign-in, billing), lake#1308 (local model), lake#1334 (image), lake#1336 (the workflows commented out) | A clean-machine build from the public repositories; the self-host first run in [`docs/deployment/`](../deployment/) |

A gate is passed when every row of it has its proof recorded in lake#1275: what was checked, the
commit, pass or fail.

## Reporting a Vulnerability

[`SECURITY.md`](../../SECURITY.md) (lake#1290) is the policy a reporter reads: email
security@orama.com, never a public issue; acknowledgement within 2 business days, a plan within 7
days, a fix or an agreed disclosure date within 90; the latest release only; the repository's code
in scope. GitHub shows it on the repository's *Security* tab. Private vulnerability reporting is a
repository setting a repository administrator switches on under *Settings → Code security*, or
with the call below. GitHub offers it only on a public repository (a private one answers 404), so
it is switched on as part of publishing:

```sh
gh api -X PUT repos/<owner>/lake/private-vulnerability-reporting
gh api repos/<owner>/lake/private-vulnerability-reporting   # {"enabled": true}
```

Change the policy by editing `SECURITY.md`; keep this page's G1 row in step.

## The Secret Scan

Two scanners run over every commit reachable from any ref, and their findings are triaged by
hand. Run them from the repository root, with every branch fetched:

```sh
git fetch origin
gitleaks git --log-opts="--all" --redact --no-banner -f json -r gitleaks.json .
trufflehog git "file://$PWD" --json --no-update > trufflehog.jsonl
```

Keep the reports out of the tree: an unredacted finding is the secret itself.

Run on 2026-11-15 (1,554 commits; gitleaks 8.30.1, trufflehog 3.99.9 with verification):
gitleaks reported 44 findings, 11 distinct values in 11 files; trufflehog reported 9, none
verified. Every one is a fixture or not a secret:

| Finding | Where | Triage |
|---|---|---|
| Stripe `sk_test_lake…fakekey…` (3 values) | `tests/test_stripe.cpp`, `tests/test_orders.cpp`, `tests/test_balance.cpp` | Made-up test keys; the tests answer from a fake Stripe |
| Clerk `pk_test_…` (2 values) | `tests/test_api.cpp`, `tests/test_http_server.cpp` | Publishable keys, public by design, encoding `example-123.clerk.accounts.dev` and `127.0.0.1` |
| Svix `whsec_…` (2 values) | `tests/test_clerk_webhooks.cpp`, `tests/test_http_server.cpp` | Test secrets encoding `lake clerk webhook test secret!!` and `someone else's secret key!!!` |
| `Authorization: Bearer …-smoke-token` (2 values) | `docs/build/scoped-credentials-proof.md`, `docs/build/tunnel-grants-proof.md` | Local smoke tokens of a throwaway instance, named as such |
| `-----BEGIN PRIVATE KEY-----` | `frontend/tests/network.test.ts` | A body of `x`, testing that a key is refused as a CSR |
| `LLVM_KEY_FINGERPRINT` | `Dockerfile` | The fingerprint of apt.llvm.org's public signing key |
| Generic key in prose and SQL | `docs/control-plane/schema.md` | Column and migration names, not values |
| Postgres URLs with passwords (8) | `tests/test_sealed_delivery.cpp`, `tests/test_workspaces.cpp`, `docs/ui/test-runbook/13-ship.md` | `pw`, `pw-app`, `s3cr3t-v1`, `hunter2` against hosts `db` and `host`; fixtures |
| URL with `user:password@` | `tests/test_instance_origin.cpp` | Against `code.example.internal`; a fixture |

No live credential was found, so the scan adds nothing to the rotation inventory. Credentials that
were shared outside Git (environment variables, Secrets, chat) are not visible to it and are
inventoried separately. Rerun both scanners on the history that is actually published, after any
rewrite, and update this table.

## The Scan

`tools/release/public-scan.sh` greps every file Git tracks for generic signs of internal material
and prints a count per category:

| Category | Pattern |
|---|---|
| `people` | an address at the company's domains, except role addresses (`support@`, `security@`, `noreply@` and the like) |
| `hosts` | a host under the development deployments' domain |
| `paths` | a local home-directory path (`/Users/<name>/`, `/home/<name>/`), except container users' homes (`/home/lake/`, `/home/pptruser/`) |
| `private-refs` | the name of a private umbrella repository; `zetta-bridge` and `liquid-bridge` are allowed, since the build names them as its sources and the public `lake-platform` repository carries them as submodules |
| `agent-files` | `CLAUDE.md`, `AGENTS.md`, `.claude/` or `.codex/` in the tree; listed, not counted, because they stay as the repository's contributor and agent instructions |

```sh
tools/release/public-scan.sh                 # the working tree's tracked files
tools/release/public-scan.sh --list          # and every matching line
tools/release/public-scan.sh origin/<branch> # another revision's tree, without checking it out
```

It exits 1 when anything matches and 0 when the tree is clean, so G3's check is that it exits 0
on the commit to be published. The patterns are generic on purpose, so the script can be
published with the tree; a release review may run a stricter private list beside it. Add a
category by adding a `name|regex` line to `categories` in the script and a row to the table above.

Measured on 2026-11-15, after the allowances above: `dev` at `2bb3a788` reports 212 lines; the
branch of lake#1317, with the G3 cleanup applied, reports 0 and exits 0.
