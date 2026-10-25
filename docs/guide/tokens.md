# Tokens and Keys

A **key** (also called an API token) lets a script, a program, an AI agent or the `liquid` command
act as you without your password. You give it a name, say what it may call, and choose how long it
lasts. It is shown once; Lake keeps only its hash.

## Concepts

| Term | What it is |
|---|---|
| **Key** or **token** | A bearer secret sent as `Authorization: Bearer <key>`. It is a named session that acts as you, limited by its scopes. The same key works for REST and MCP. |
| **Scope** | What the key may call: a capability name (such as `search_index`), `*` for every capability, or `registry_pull` to pull images with `docker login`. |
| **Read-only key** | A key whose scopes are every read capability; every write is refused. |
| **Lifetime** | How long the key works. By default as long as the sign-in that made it, at most 30 days. |
| **Standing key** | A key that never expires. It needs a reason, named scopes and your account's opt-in. Every key survives sign-out (lake#1163); a standing key also outlives the browser session's expiry. |
| **Static key** | A key not tied to a sign-in session. It needs a reason and can only be made from a credential that is already static. |

A key can **never do more than you**: its scopes limit which capabilities it may call, and your own
grants still decide which objects ([Grants and Effective Access](/docs/access)).

## Where Keys Are Made

Three pages make keys, all with the same rules, and every key appears on all of them:

- **Developer → API**, the **Keys** tab: **Create an API key**.
- **Developer → MCP**, step one: **Create an MCP key** (and **Your Keys** further down).
- **Tokens** in the account menu (top right): **New token**.

## Create a Key on Developer → API or MCP

1. Open **Developer → API** (the **Keys** tab) or **Developer → MCP**.
2. Enter a **Name** that says where it will be used, such as *Nightly export* or *Claude Code on my
   laptop*.
3. Choose the **Lifetime**: *As long as my sign-in allows (at most 30 days)*, *1 day*, *7 days*, *A
   number of seconds*, or *Never (a standing key)*.
4. Choose the **Access**:
   - **Every operation** (**Every tool** on MCP): everything you may do.
   - **Read-only** (**Read-only tools**): only reads; every write is refused.
   - **Named capabilities** (**Named tools**): only the capabilities you list, comma separated.
5. For a standing or static key, fill in **Why this key exists**.
6. Press **Create an API key** (or **Create an MCP key**).
7. Copy the key from the callout, store it, and press **I've stored it**. It is not shown again.

- REST: `POST /v1/create_token` with
  `{"name": "Nightly export", "scopes": ["list_repositories", "get_repository"], "expires_in_seconds": 604800}`.
- MCP: `create_token` with the same input.

The answer's `secret` is the key. Leave out `expires_in_seconds` for the default lifetime.

## Create a Token on the Tokens Page

1. Open the account menu (top right) and choose **Tokens**. The **API Tokens** page lists your
   tokens with their scopes and expiry.
2. Press **New token**.
3. Enter the **Name** and the **Capability scopes, comma separated**. For `liquid` push and pull,
   press the suggested scope list to fill in `refs,find_missing,push,pull,upload_chunks,read_chunk`.
4. Optionally set **Expiry in seconds** (empty means as long as this session allows, at most 30
   days), tick **Static exception** or **Never expires**, and give a reason.
5. Press **Create token**, and copy it from *Copy this token now. It is not shown again.*

## Keys for the liquid Command

`liquid clone`, `liquid pull` and `liquid push` read your key from `LIQUID_TOKEN`. Make a key with
the push and pull scopes:

- REST: `POST /v1/create_token` with
  `{"name": "liquid on my laptop", "scopes": ["refs", "find_missing", "push", "pull", "upload_chunks", "read_chunk"]}`.

Then `export LIQUID_TOKEN=<the key>` before running `liquid`. The key keeps your repository
permissions. See [The liquid CLI](/docs/cli).

## Search-Only Keys

A published search index can be called from outside Lake with a key that can do nothing else. On
**Ship → Search Indexes**, open the index and use **Create a search key**; it makes a key scoped to
`search_index` only ([Models and Search Indexes](/docs/ship)). It works for any published index
you can read until you revoke it, and expires no later than the session that made it would have,
at most 30 days. Signing out does not end it.

- REST: `POST /v1/create_token` with `{"name": "Website search", "scopes": ["search_index"]}`.

## Standing Keys That Never Expire

Every key keeps working after you sign out (lake#1163). A standing key also never expires, for a
service that runs unattended. It needs:

- a non-empty reason;
- named scopes (never `*`; an *every tool* standing key made on the Developer pages names every
  capability Lake has today);
- your account's opt-in, which an admin of the account turns on;
- the Lake instance allowing it (an operator can turn standing keys off for the whole Lake).

Turn on the opt-in (needs `admin` on the account):

- REST: `POST /v1/set_customer_credential_policy` with
  `{"customer_id": "<customer id>", "allow_non_expiring_credentials": true}`.
- MCP: `set_customer_credential_policy`.

Then make the key:

- REST: `POST /v1/create_token` with
  `{"name": "Warehouse sync", "scopes": ["workspace_query", "workspace_sql"], "never_expires": true, "reason": "the nightly sync from our data warehouse"}`.

Its authority is still your live grants on every request: a grant you lose is lost to the key too.
Revoking the key, or an operator ending all of your sessions, stops it.

## List and Revoke Keys

Every page that makes keys lists them: name, access, expiry, last used, created and state.

1. Find the key in the table.
2. Press **Revoke** and confirm.

- REST: `GET /v1/list_sessions` lists your sessions and tokens; `POST /v1/revoke_session` with
  `{"id": "<token id>"}` revokes one.
- MCP: `list_sessions`, then `revoke_session`.

A revoked key stops on its next request. Sign-in sessions and devices are on **Sessions** in the
account menu.

## The Rules a Key Lives By

- **It never outlives what made it.** A key made from a browser session lasts at most as long as
  that session (and at most 30 days). Revoking the session, or any credential above it, or the
  device it came from, stops every key made from it on its next request.
- **It never gains scopes.** Each scope you ask for must be one the issuing credential has. A
  named-scope credential cannot make a `*` key; a longer lifetime than the issuer has left is
  refused.
- **Unknown scopes are refused,** so a typo cannot hide: every scope must be a capability name
  (listed as `operationId` in `/v1/openapi.json`), `*` or `registry_pull`.
- **Every call is audited** under your name, as if you had made it on the site.
- **No read returns a secret.** A key is shown once, when it is made.

The operator has no tokens: its key is the instance's admin token. The Developer pages say so
instead of offering the form.

## Using a Key

```
curl -s https://lake.oramadev.net/v1/list_repositories?limit=5 \
  -H "Authorization: Bearer $LAKE_KEY"
```

For MCP clients, see [MCP Setup](/docs/mcp); for REST, [REST API and OpenAPI](/docs/developer).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| 401 `unauthenticated` with `reason` `missing` | No `Authorization: Bearer` header was sent. |
| 401 with `reason` `expired` | The key's lifetime ended, or the sign-in it came from expired. Make a new key. |
| 401 with `reason` `invalid` | The key is mistyped, revoked, or its parent session or device was revoked. |
| 403 `unauthorized` | The key is valid but its scopes do not include this capability, or you have no grant on the object. |
| `invalid_input` at `/scopes/0` | A scope is not a capability name, `*` or `registry_pull`. |
| `unauthorized` when making a key | You asked for a scope or lifetime the credential you are using does not have. |
| A standing key is refused | Your account has not opted in, the scopes include `*`, the reason is empty, or the instance does not allow standing keys. The message names what refused it. |

## Related Pages

- [REST API and OpenAPI](/docs/developer)
- [MCP Setup](/docs/mcp)
- [The liquid CLI](/docs/cli)
- [Your Account and Sign-In](/docs/account)
- [Grants and Effective Access](/docs/access)
- [Security Model](/docs/security)
