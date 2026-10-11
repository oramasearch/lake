# Security Model

How lake decides who you are, what you may do, where your data is kept and how you can check what
happened. Read this before you hand a key to a script or an agent, share a folder, or answer a
security review about lake.

## The Short Version

- **One way in.** The browser's session cookie and an API key are checked by the same code on
  every request. Revoking either takes effect on the next request.
- **One permission check.** Every call, from the web app, REST, MCP or an agent, passes through one
  capability registry and one policy engine before anything runs. The web app can do nothing the API
  cannot.
- **Everything is recorded.** Every call, including refusals, is a signed, hash-linked audit event
  you can read and verify.
- **Your data stays in your workspace.** What you put in a workspace lives in that workspace
  itself, sealed with the workspace's key.
- **Agents act as you, and ask first.** An agent's turn holds a short-lived key that can read but not
  change; every change it wants is a proposal you confirm.

## Concepts

| Term | What it is |
|---|---|
| **Session** | What signing in creates: a random secret lake stores only as a digest. The browser holds it in the `lake_session` cookie. |
| **Key** (API token) | A named session for programs and agents, sent as `Authorization: Bearer <key>`, limited to the capabilities it names. |
| **Device** | A browser or passkey that signed in. Revoking a device ends its sessions and its passkey. |
| **TAAP identity** | Your cryptographic identity. Grants, audit events and attestations name it. |
| **Policy object** | Anything that can be granted on: an organization, a team, a repository or a path in it, an environment, a secret mount. Objects form a tree, and grants are inherited down it unless an object cuts inheritance. |
| **Role** | What a grant allows: `read`, `write`, `review`, `deploy`, `share` or `admin`, depending on the object. |
| **Audit log** | The signed record of every call: the central log for sign-in, identity and access changes, and each workspace's own log for its data. |
| **Transparency log** | lake's public, append-only Merkle log of signed attestations, with signed checkpoints anyone can check. |

## Signing In

You sign in with a password (and a TOTP code when MFA is on), a passkey, an OAuth provider, your
organization's single sign-on, or, on lake instances that use it, Clerk. Each ends in the same kind
of session.

- The session secret is set as an `HttpOnly` cookie, `SameSite=Lax`, and `Secure` whenever the
  request came over HTTPS. Page scripts never see it, and lake never puts it in a URL.
- Passwords and identity documents travel only in request bodies, never in an address.
- When a session ends or is revoked, the page says *Your session has ended* and offers **Sign in
  again**, which brings you back to the same page.
- **Sign out** in the account menu revokes the session and clears the cookie.

### Turn On Multi-Factor Authentication

1. Open the account menu → **Settings**. The Profile tab shows whether MFA is enabled.
2. Press **Manage MFA**, then **Enroll or replace MFA**.
3. Add the authenticator URI to your authenticator app and store the recovery codes. Both are shown
   once, held only in that page, and gone when you leave it. Enrolling again replaces them.
4. To turn it off, enter a current code or an unused recovery code and press **Disable MFA**.

From then on, password sign-in asks for a code. Without MFA, an action that requires it answers
`mfa_required`.

### Register a Passkey

Open the account menu → **Passkeys** and press **Register passkey**. A registered passkey appears as
a device on **Sessions**, where you can revoke it. lake signs a binding between the passkey and
your TAAP identity, so the binding can be checked later. (Passkeys are not offered on lake instances
that sign in through Clerk.)

### Review and Revoke Sessions

Open the account menu → **Sessions**. This browser's session is listed first; other sessions,
keys and devices follow. Revoking takes effect on the next request; revoking this browser's session
signs you out.

The same through the API: `GET /v1/list_sessions` lists your sessions and keys, and
`POST /v1/revoke_session` revokes one by its `id`:

```json
{"id": "<session id>"}
```

`GET /v1/list_devices` lists your devices, and `POST /v1/revoke_device` revokes one with its
sessions and passkey:

```json
{"id": "<device id>"}
```

The MCP tools have the same names. You can only list and revoke your own.

## Keys and Their Limits

A key is for a program or an agent. Make one on **Developer → API**, **Developer → MCP** or
**Tokens** in the account menu ([Tokens and Keys](/docs/tokens)). A key is bounded three ways:

- **Scopes.** It may call only the capabilities it names, `*` for every capability, or
  `registry_pull` to pull images. An unknown name is refused when the key is made, so a typo never
  hides.
- **Attenuation.** A key can never hold more than the credential that made it: its scopes must be a
  subset of yours, and its expiry must fit inside your sign-in's remaining lifetime.
- **Revocation follows the chain.** Revoking a sign-in, or the device it came from, revokes every key
  made from it.

The longest ordinary lifetime is 30 days. A standing key that never expires needs a reason, must
name its capabilities (it cannot be `*`), and needs the opt-in of the account that owns your
workspaces. For example, `POST /v1/create_token` for a seven-day read-only key:

```json
{"name": "warehouse reader", "scopes": ["list_repositories", "get_repository", "workspace_sql"], "expires_in_seconds": 604800}
```

The secret is shown once; lake keeps only its hash.

## Cross-Site Requests

Ship apps are served on addresses next to lake's own, so a page of an app is *same-site* with lake
and a browser would send lake's cookie along with its requests. lake therefore refuses any unsafe
request (`POST`, `PUT`, `PATCH`, `DELETE`) authenticated only by the `lake_session` cookie when:

- `Sec-Fetch-Site` is present and is not `same-origin`;
- `Origin` is present and is not lake's own origin (including `Origin: null`); or
- it is a `POST` to `/v1/<capability>` or `/mcp` whose `Content-Type` is not `application/json`.

The refusal is `403 unauthorized`: *Cross-site requests cannot use the browser session.* Calls with
`Authorization: Bearer` are not affected, from any origin. A script, an app or a tool that calls lake
sends a key, never a copied cookie.

## Permissions

Every protected call resolves the object it acts on and the action it needs, then asks one policy
engine whether your identity has it. Lists are filtered the same way, item by item, so a list never
shows what you could not open.

- **Roles.** `read` reads; `write` reads and changes; `review`, `deploy` and `share` read plus their
  named action; `admin` allows all. Which roles apply depends on the object: a repository has
  `review`, an environment `deploy`.
- **Inheritance.** A grant on an organization reaches its repositories and their paths. An object can
  cut inheritance, so grants above it stop applying there.
- **Where you see it.** **Organization → Organizations** shows members, teams and invitations; a
  repository's and an organization's **Access** shows who has access, directly and inherited, and
  through what. [Grants and Effective Access](/docs/access) has the steps.
- **Refusals.** A call you may not make answers `403 unauthorized`. An object you cannot see answers
  `404 not_found`, and a private repository opened while signed out sends you to sign in, so neither
  tells an outsider whether it exists.

Who has access to one object, with the source of each grant, is
`GET /v1/effective_access?object_id=<object id>` (it needs `share` on the object).

## Where Your Data Lives

- **Workspace data stays in the workspace.** Datasets, runs, models, transcripts, Ask Orama
  conversations, repository descriptions, issues and reviews, app settings, secrets and the
  workspace's own audit are records in that workspace, never in lake's central database.
- **Sealed.** lake seals each record with the workspace's key (AES-256-GCM) and finds it by a keyed
  hash of its name, so the workspace's storage holds only ciphertext. A record copied to another
  workspace or another place does not open.
- **Central data is about access only**: accounts and their workspaces, memberships and grants,
  sign-in and sessions, and the routing that says which workspace holds what.
- **Deleting a workspace deletes its data.** Every record in it, its repositories, apps,
  builds and images go with it; the confirmation lists them first. See
  [Workspaces](/docs/workspaces).
- **Recordings are never kept.** Audio sent for speech-to-text is held in memory for the one call,
  sent to the speech-to-text provider and dropped; only the text is stored. See
  [Transcripts and Speech-to-Text](/docs/transcripts).
- **Secrets are never shown back.** The Secrets pages show names, versions and metadata, never a
  value or key material. A new key, an MFA secret and recovery codes are shown once. See
  [Secrets and Encryption Keys](/docs/secrets).

## Agents

- **Ask Orama works as you.** Each turn gets a key that exists for that turn only, derived from your
  own credential, carrying every read capability and no action. It is revoked when the turn ends.
- **Changes are proposals.** When an agent wants to change something, it shows a proposal; only
  **Confirm** runs it, as you, through the ordinary permission check and audit. Only a proposal lake
  stored in your own conversation can be confirmed.
- **Builders show the exact result first.** ApplicationAgent and ExperimentsAgent show the complete
  app or experiment before anything is written, and only a second turn carrying the exact proposal you
  reviewed gets the write tools. A public app needs its own confirmation.
- **The audit names the agent.** An agent acting for you is recorded with your identity and the
  agent's name, for example *Project Agent for Ada Lovelace*.

See [Ask Orama](/docs/ask-orama) and [Agents](/docs/agents).

## Public Repositories and Share Links

- A repository is private unless it is made public. A signed-out visitor sees only public
  repositories; any other address sends them to sign in without saying whether it exists.
- A share link opens one folder, read-only or with the role it was made with, until its required
  expiry. lake keeps only a digest of the link's token. Every open, preview and download checks the
  link's scope, role, expiry and revocation again, and a guessed parent or sibling folder is
  refused. Revoking a link stops its next use; a copy someone already downloaded cannot be recalled.

See [Sharing and Shared with Me](/docs/sharing).

## Audit

Every capability call is recorded: who (the TAAP identity, and the agent if one acted for them),
what, on which object, the outcome (including denials and invalid input) and when. Events are
signed with lake's identity, and each names the digest of the event before it, so a removed or
changed event breaks the chain.

- **Two kinds of log.** Sign-in, sessions, keys, organizations, memberships, grants and workspaces
  themselves go to the central log. Events about a workspace's data go to that workspace's own log,
  kept in the workspace, so they are deleted with it. Refused calls stay in the central log.
- **Your own events.** **Organization → Activity** shows what you did, newest first, with filters,
  **Include reads**, and the log each event came from. The operator's **Administration → Audit**
  shows everyone's.
- **Retention.** The central log is kept at least 400 days and cannot be shortened; a workspace's log
  is kept until the workspace is deleted.

Your own events through the API, `GET /v1/my_audit?limit=50&include_reads=false`. Check a log's
chain, signatures and digests with `GET /v1/verify_audit` (the central log) or
`GET /v1/verify_audit?workspace_id=<workspace id>` (one workspace's log). A broken chain answers
`invalid_chain`, `invalid_signature` or `invalid_digest`, naming the place.

What the chain cannot show by itself: the newest events of a workspace's log being removed, or a
whole workspace rolled back by whoever runs its storage.

## Transparency

lake appends signed attestations (for example agent delegations and secret lease lifecycles) to an
append-only Merkle log, and publishes signed checkpoints that bind its size and root. Checkpoints
can require witness cosignatures before they become current. Anyone can check, without a key:

- `GET /v1/transparency_checkpoint`: the current signed checkpoint.
- `GET /v1/transparency_identity_scan?identity=<TAAP identity>`: every attestation that names an
  identity, after recomputing the whole log's root; a changed or missing entry fails the scan rather
  than returning partial results. The **Transparency Monitor** page (`/transparency`) runs it: enter
  an identity and press **Scan**.
- `GET /v1/transparency_proof_bundle?leaf_index=0`: a self-contained bundle (checkpoint, tiles and an
  inclusion proof) that liquid's verifier checks offline, with no further call to lake.

Two different checkpoints for the same log size are evidence of a split view, and liquid's verifier
refuses the second.

## Troubleshooting

- **`unauthenticated`**: the `reason` says `missing`, `expired` or `invalid`. Sign in again, or make
  a new key.
- **`unauthorized` from a script that worked in the browser**: the script sent the cookie. Send a
  key instead ([Cross-Site Requests](#cross-site-requests)).
- **`mfa_required`**: finish the code step, or turn on MFA first.
- **A key cannot be made with the scopes you asked for**: a key cannot exceed your own access or
  your sign-in's lifetime, and a key that never expires cannot be `*`.
- **`invalid_chain` from `verify_audit`**: an event is missing or changed. Report it with the place
  named in the message.
- More answers: [FAQ and Troubleshooting](/docs/faq).

## Related Pages

- [Your Account and Sign-In](/docs/account)
- [Tokens and Keys](/docs/tokens)
- [Grants and Effective Access](/docs/access)
- [Activity and Audit](/docs/activity)
- [Single Sign-On and SCIM](/docs/sso)
- [Secrets and Encryption Keys](/docs/secrets)
- [REST API and OpenAPI](/docs/developer)
