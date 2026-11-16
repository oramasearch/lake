# Releases

One page per tagged release, newest first. A page states what the release includes, what it
does not, what is knowingly still broken, and how it was verified.

- [v0.9.0](v0.9.0.md) — 2026-11-16. The open-source release: MIT licence, the pre-launch security
  fixes, lake on liquid v0.3.0, and the public README.
- [v0.8.1](v0.8.1.md) — 2026-10-27. A patch: a file's exact text, clustering on real columns,
  predicting on a clustering model, Change App reloading onto the new version, and end-to-end fixes.
- [v0.8.0](v0.8.0.md) — 2026-10-26. Agents on gpt-6.1-sol again (gpt-6.x tool calls through the Responses API;
  lake#1245); edit repository files, a change-App conversation and the Change App split screen (lake#1239–#1241).
- [v0.7.3](v0.7.3.md) — 2026-10-26. A hotfix: agents default to gpt-5.5 again (gpt-6.x refuses tools with
  `reasoning_effort` on Chat Completions; lake#1245).
- [v0.7.2](v0.7.2.md) — 2026-10-26. A patch: an App's `workspace.sql` at its binding's access, read-only and
  capped at 5,000 rows (`too_many_rows`); auto-fix sees a healthy App whose script keeps failing, and Restart is
  recorded (pods replaced with orama-operator#1033); people choose the agents' model per workspace (default
  gpt-6.1-sol), the gpt-6.x models served and priced, long prompts billed at cost. Migrations 0104 and 0105, no
  new settings.
- [v0.7.1](v0.7.1.md) — 2026-10-25. A patch: the Quickstart's model step as an Ask Orama conversation, training
  on any column types, clustering that trains and one next step in every state, and a copy-ready agent prompt; a
  failed app as a notification, fixed or changed with the Application Agent, with opt-in auto-fix, and Degraded
  apps counted as needing attention; a readable Ask Orama confirm card and no proposal that cannot work;
  Auto-Suggest finds opportunities again and a scan survives leaving the page. No migrations, no new settings.
  Ships with agent-ask-orama and agent-application v0.2.1.
- [v0.7.0](v0.7.0.md) — 2026-10-24. Quickstart (a dropped dataset to search and model answers on your
  website) with browser keys, credits from Orama and coupon codes, sign-out that keeps API keys, a
  white-labelled Lake's domain, sign-in and brand, `lake migrate` and `lake manifest`, and security
  and memory fixes. Migrations `0100` to `0103`.
- [v0.6.1](v0.6.1.md) — 2026-10-24. A patch: a blocked account can stop or rename its workspace (#1122).
- [v0.6.0](v0.6.0.md) — 2026-10-24. The hosted service becomes prepaid: an account balance with Stripe top-ups,
  warnings and emails, and a stop on paid work at $50 below zero; sign-in with no name in Clerk.
  Migration `0099`.
- [v0.5.2](v0.5.2.md) — 2026-10-24. A patch: request headers up to 1 MiB, so people can sign in with
  Google (#1108).
- [v0.5.1](v0.5.1.md) — 2026-10-24. A patch: lake installs the operator's `ZETTA_EGRESS_ALLOW`, so it
  reaches its Zettas (#1097). Needs orama-operator v0.9.2.
- [v0.5.0](v0.5.0.md) — 2026-10-24. The rest of the pre-launch security review and the liquid#383
  identity fix, safe mode per database with a dialog showing the exact query, model prices, and
  stricter sign-in, recovery and request limits. Built against zetta v0.5.0. Five migrations (`0094`
  to `0098`) the operator vendors before the image rolls.
- [v0.4.0](v0.4.0.md) — 2026-10-21. The security release from the multi-tenant audit and the
  pre-launch review: every Critical the review found in Lake closed, SAML sign-in with Lake's own
  signature verification, OpenID Connect discovery, `delete_account`, and 23 more security fixes.
  Three migrations (`0091` to `0093`) the operator vendors before the image rolls.
- [v0.3.1](v0.3.1.md) — 2026-10-20. A patch for billing through a Stripe account shared with Cloud v1
  and other Lakes: Lake's objects tagged and others' events ignored, invoices and Checkout under
  Lake's own products, a missing kept customer replaced, billing contacts (migration `0090`) with a
  card on the Billing page, and invoices emailed by Stripe. Image `main-89968edc` = `v0.3.1`
  (`sha256:d2347fb0…`), on every Lake since 2026-10-20.
- [v0.3.0](v0.3.0.md) — 2026-10-19. The order sheet (four offers, Stripe Checkout,
  order forms, Resend email, status page; migration `0089_cloud_orders`), and the 2026-10-18 UX
  review's fixes: datasets named after their file and shown in the file's column order with types,
  a dataset page that leads to a model, short pipeline and model names, Check after Approve, and
  Ask Orama proposals the capability accepts.
- [v0.2.0](v0.2.0.md) — 2026-10-17. Secure networking (connections, scoped credentials, egress,
  developer tunnels and the Network section), regions and the apex picker, a managed key for
  every new account with an optional own identity, Clerk webhooks, one name per agent with all
  nine agents offered, and Ask Orama's home. Image: `oramasearch/lake:v0.2.0` (the same digest as `main-508f9540`,
  `sha256:e41273a7…`).
- [v0.1.0](v0.1.0.md) — 2026-10-12. The first tagged release, so the page is the full inventory:
  every feature by area, all 554 deployed capabilities by name, every page of the web app and
  every guide. Image:
  `oramasearch/lake:v0.1.0` (the same digest as `20261012a`).

From v0.7.0 the image applies its own schema (`lake migrate`) and declares its Stripe webhook events,
Stripe products and environment (`lake manifest`), so the operator needs no vendored copy of them.
Releases up to v0.6.1 predate the commands.

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
