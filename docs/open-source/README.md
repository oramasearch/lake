# Open-Source Release

lake is published under the MIT licence as part of the Orama Lake open-source release. Before it
is published, the tree must pass five gates: security, secrets, internal material, licences, and
usable from public sources alone. This page records what each gate needs, what covers it, and how
to check it. The tracking issue is lake#1275.

## Gates

| Gate | What it needs | Covered by | Check |
|---|---|---|---|
| G1 Security | No open `security` issue; a private way to report one; a final review of the commit to be published | [`SECURITY.md`](../../SECURITY.md) (this page's change); the open `security` issues | `gh issue list --label security --state open`; the repository's *Private vulnerability reporting* setting is on |
| G2 Secrets | A second secret scanner over the tree and the full history, every finding triaged, every credential that ever appeared rotated; test fixtures and smoke tokens confirmed not live | Not started | The scanner's report, triaged by a person |
| G3 Internal material | No internal names, hosts, paths, agent instruction files, private-repository references, quoted decisions or internal pricing in the tree | lake#1320, lake#1317, lake#1322 | [`tools/release/public-scan.sh`](#the-scan) comes back empty |
| G4 Licences | `LICENSE` (MIT), `COPYRIGHT`, an SPDX header in every source file we wrote, a generated `THIRD_PARTY_LICENSES` with nothing incompatible | lake#1321, lake#1334 | Every file in the tree we wrote carries `SPDX-License-Identifier: MIT` |
| G5 Usable | Builds and passes its tests from public sources on a clean machine; README, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`; a release with notes; a public, signed image with an SBOM; no default pointing at an Orama endpoint; native sign-in, billing off and a local model as the self-host defaults | lake#1321 (contributor docs), lake#1307 (sign-in, billing), lake#1308 (local model), lake#1334 (image) | A clean-machine build from the public repositories; the self-host first run in [`docs/deployment/`](../deployment/) |

A gate is passed when every row of it has its proof recorded in lake#1275: what was checked, the
commit, pass or fail.

## Reporting a Vulnerability

[`SECURITY.md`](../../SECURITY.md) is the policy a reporter reads: GitHub private vulnerability
reporting first, an email address otherwise, the response times, the supported versions and the
scope. GitHub shows it on the repository's *Security* tab. Private vulnerability reporting is a
repository setting, switched on by a repository administrator under *Settings → Code security*
or with:

```sh
gh api -X PUT repos/<owner>/lake/private-vulnerability-reporting
gh api repos/<owner>/lake/private-vulnerability-reporting   # {"enabled": true}
```

Change the policy by editing `SECURITY.md`; keep this page's G1 row in step.

## The Scan

`tools/release/public-scan.sh` greps every file Git tracks for generic signs of internal material
and prints a count per category:

| Category | Pattern |
|---|---|
| `people` | an address at the company's domains, except role addresses (`support@`, `security@`, `noreply@` and the like) |
| `hosts` | a host under the development deployments' domain |
| `paths` | a local home-directory path (`/Users/<name>/`, `/home/<name>/`) |
| `private-refs` | the name of a private umbrella or bridge repository |
| `agent-files` | `CLAUDE.md`, `AGENTS.md`, `.claude/` or `.codex/` in the tree |

```sh
tools/release/public-scan.sh                 # the working tree's tracked files
tools/release/public-scan.sh --list          # and every matching line
tools/release/public-scan.sh origin/<branch> # another revision's tree, without checking it out
```

It exits 1 when anything matches and 0 when the tree is clean, so G3's check is that it exits 0
on the commit to be published. The patterns are generic on purpose, so the script can be
published with the tree; a release review may run a stricter private list beside it. Add a
category by adding a `name|regex` line to `categories` in the script and a row to the table above.

Measured on 2026-11-15: `dev` at `2bb3a788` reports 341 lines (people 9, hosts 148, paths 23,
private-refs 159, agent files 2); the branch of lake#1317, with the G3 cleanup applied, reports 36
(paths 2, private-refs 32, agent files 2).
