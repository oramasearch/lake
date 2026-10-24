# Activity and Audit

Every call to Lake, from the web app, REST or MCP, is recorded as a **signed audit event**: who
acted, on what, with which capability, and whether it succeeded, was denied or failed. Refused calls
are recorded too. Events are hash-linked, so a removed or altered event is detected.

**Organization → Activity** shows your own events. The Lake operator's **Administration → Audit**
shows everyone's.

## Concepts

| Term | What it is |
|---|---|
| **Event** | One recorded call: when, the principal (with a readable name), the action and, for a workspace call, the inner capability, the object, the outcome, and the event's digest. |
| **Outcome** | `success`, `denied` (no permission) or `failure` (an error). |
| **Central log** | Lake's own log, for sign-in, sessions, tokens, users, organizations, memberships, grants, workspaces themselves, SSO and SCIM, and other control-plane events. |
| **Workspace audit** | Each workspace's own log, kept in the workspace itself, for events about that workspace's data: datasets and SQL, repositories and pushes, apps, builds, secrets, streams, agents, experiments, Ask Orama and Design. |
| **Chain** | Each log is a hash chain: every event carries the digest of the one before it and Lake's signature. |
| **Transparency log** | An append-only public log of signed statements (such as agent session delegations and dynamic-secret lease events), with signed checkpoints anyone can check. |

A call refused before it ran (no credential, invalid input, no permission) is always recorded in the
central log, never in a workspace. An event about a workspace reaches that workspace's audit within
about a second of the call; until then the page shows it as *being written*.

## See Your Own Activity

1. Open **Organization → Activity**. **Your Activity** lists your events, newest first, from the
   central log and the audits of your workspaces.
2. Use the outcome chips (**Succeeded**, **Denied**, **Failed**) to narrow it.
3. Successful reads (the polling pages do) are hidden by default. Turn on **Include reads** to see
   them; failed and denied reads always show.
4. Press **Filters** for **Object** (such as `repository/<id>`), **Outcome**, **From**, **To** and
   **Order** (**Newest first** or **Oldest first**), then **Filter**.

Each row shows the time, the action and capability, who acted, the object, the outcome, the log it
came from (**Central** or **Workspace** and its name) and the event's digest. If a workspace's audit
could not be read, a banner names it and the rest is still shown.

- REST: `GET /v1/my_audit?limit=50&include_reads=false`.
- MCP: `my_audit` with the same input.

`my_audit` always returns only your own events; a `principal` filter cannot widen it. Add
`workspace_id` to read one workspace's events only, or `log=central` for the central log alone.

When an agent acts for you (the Project Agent, an agent you built, Ask Orama confirming a proposal),
the event names it, such as *Project Agent for Ada Lovelace*.

## See an Organization's Events

Open the organization from **Organization → Organizations** and choose the **Audit** tab. It shows
the organization's events from the central log, with outcome chips.

## Audit Log for the Operator

The Lake operator sees **Administration → Audit**: every event on the Lake, with an extra
**Principal** filter and a **Transparency log** card showing the log's mode and witness quorum.

- REST: `GET /v1/audit?limit=100&outcome=denied`.
- `customer_id` limits the workspaces read to one account's; `workspace_id` reads one workspace's
  audit; `request_id` finds the events of one request; `after_id` continues after an event.

## Verify and Export a Log

Verification checks every signature, digest and link of a chain. Export returns the events as JSON
lines, one event per line, each naming its log.

- Verify the central log: `GET /v1/verify_audit`.
- Verify one workspace's chain: `GET /v1/verify_audit?workspace_id=<workspace id>`.
- Export: `GET /v1/export_audit?workspace_id=<workspace id>`, or without `workspace_id` for the
  central log in the order it was written.
- MCP: `verify_audit` and `export_audit`.

A broken chain answers `invalid_chain`, a bad signature `invalid_signature` and a changed event
`invalid_digest`, each naming the place in the chain.

Verification detects a removed or changed event inside a chain. It does not detect the newest events
of a workspace being removed from its end, or a whole workspace being rolled back.

## Retention

The central log is kept for at least **400 days**. An account admin can lengthen it, never shorten
it:

- REST: `POST /v1/set_audit_retention` with `{"customer_id": "<customer id>", "days": 730}`.
- MCP: `set_audit_retention`.

A value below 400 days or below the current setting is refused, and the refusal is itself recorded.
A workspace's audit is kept as long as the workspace exists and is deleted with it. When a workspace
is deleted, Lake records one central event per repository, app, build and image it destroyed, so
that history survives.

## The Transparency Log and Monitor

Lake appends signed statements to a public, append-only transparency log and publishes signed
checkpoints of it. Anyone can check that a statement naming an identity is in the log:

1. Open the **Transparency Monitor** at `/transparency` (no sign-in needed).
2. Enter a **TAAP identity** (`taap:1:…`) and scan.

The page verifies the current checkpoint's root over the log's tiles and lists the attestations
that name that identity, 100 a page; **Next** opens the following page. Modified tiles are refused.

- REST: `GET /v1/transparency_identity_scan?identity=<TAAP identity>`.
- MCP: `transparency_identity_scan`.

## Permissions

| Call | Who |
|---|---|
| `my_audit` | Any signed-in person, for their own events |
| `audit`, `verify_audit`, `export_audit` | The Lake operator |
| `set_audit_retention` | `admin` on the account |
| `transparency_identity_scan` | Anyone |

## What Is Never in an Event

Secret values, plaintext and ciphertext, passwords, token secrets and transcript text are never
recorded. A workspace call records its capability and identifiers, not its data.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| Your activity looks empty | Successful reads are hidden; turn on **Include reads**, or widen **From** and **To**. |
| *The audit of workspace … could not be read* | That workspace is not ready or could not be reached. The other logs are shown; try again once the workspace is ready. |
| An event shows *being written* | It is queued for its workspace's audit and has no signature yet. It is written within about a second. |
| `unauthorized` on `audit` | Only the operator reads the whole log; use `my_audit` for your own. |
| `invalid_input` on `set_audit_retention` | The value is below 400 days. |
| `conflict` on `set_audit_retention` | The value is below the current setting; retention can never be reduced. |
| `invalid_chain`, `invalid_signature` or `invalid_digest` | The log was changed. Keep the export and contact the Lake operator. |

## Related Pages

- [Security Model](/docs/security)
- [Grants and Effective Access](/docs/access)
- [Organizations, Members and Teams](/docs/organizations)
- [Secrets and Encryption Keys](/docs/secrets)
- [Usage and Metering](/docs/usage)
