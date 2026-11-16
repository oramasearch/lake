# Browser keys

lake#1152. The key a website's own page holds, so the page can search a published index (and ask a
published model) of one workspace.

## Why

A key in a public web page is read by everyone who opens the page. Before browser keys, the
quickstart (lake#1128) and Ship → Search Index handed out an ordinary API token scoped to
`search_index` (and `predict`). That token:

- could search every published index and ask every published model of every account its maker can
  read, from any website;
- ended with the sign-in that made it (at most 30 days), so the customer's website broke when they
  signed out;
- could not be made by an agent on an API token, since `create_token` needs a signed-in session.

A browser key is narrower and lasts as long as the website needs it.

## What a browser key is

| | |
|---|---|
| Bound to | one workspace of one account, one search index in it, and optionally one model in it |
| May call | `search_index` on its index; `predict` on its model, when it names one. Nothing else |
| From | the website origins it lists (1 to 20): `https://host[:port]`, or `http://localhost[:port]` and `http://127.0.0.1[:port]` while trying the code. Lake checks the call's `Origin` header |
| CORS | a call with the key is answered with `Access-Control-Allow-Origin` set to the calling origin when the key lists it, and with none otherwise, so another site's page cannot read the answer |
| How often | `rate_limit_per_minute` calls a minute (600 unless set, 1 to 6000), counted across every Lake replica; past it, `429 rate_limited` |
| How long | until it is revoked (`revoke_browser_key`), or until its optional `expires_in_seconds` (60 s to 10 years). It names no session: signing out, or the maker's tokens ending, leaves it working. Deleting its workspace ends it |
| Billed | every search and answer counts in the workspace's usage as any `search_index` and `predict` call does (`searches`, `predictions`, `calls`), billed to the account |
| Acts as | no person. Its audit principal is `browser_key:<id>`. Policy grants are not consulted for its calls: its binding is what it may do |
| Secret | `lbk.<key id>.<random>`, answered once by `create_browser_key`; Lake keeps only its SHA-256 |

Anyone can copy the key out of the page and call Lake with it from a script, setting `Origin` by
hand. What they can do is still only what the key is bound to: search that one index and ask that
one model, at its calls a minute, billed to the account. The origin check stops other websites
from using it in their visitors' browsers; it is not a secret. Revoke a key that is abused and make
another.

## Capabilities

| Capability | Kind | Authorized as | Input | Answer |
|---|---|---|---|---|
| `create_browser_key` | action | write on the account | `customer_id`, `workspace_id`, `index_id`, `origins`; `model_id?`, `name?`, `expires_in_seconds?`, `rate_limit_per_minute?` | `key` (below), `secret` (once), `index_published`, `model_published` |
| `list_browser_keys` | read | read on the account | `customer_id`; `workspace_id?`, `index_id?` | `keys`, newest first, revoked and expired ones too |
| `revoke_browser_key` | action | write on the account | `customer_id`, `id` | `key`; revoking a revoked key changes nothing |

A key as they answer it (`key` in each answer, an item of `keys`):

| Field | |
|---|---|
| `id`, `name` | the key's id (also in its secret) and its name (`website <first origin>` unless named) |
| `customer_id`, `workspace_id`, `index_id`, `model_id` | its binding; `model_id` is null when it asks no model |
| `origins` | the websites it works from, such as `["https://shop.example.com"]` |
| `capabilities` | `["search_index"]`, or `["search_index", "predict"]` with a model |
| `rate_limit_per_minute` | its calls a minute |
| `state` | `active`, `revoked` or `expired` |
| `created_by`, `created_at`, `expires_at`, `revoked_at`, `last_used_at` | who made it (a TAAP identity) and when; times are RFC 3339 UTC, null when not set |

`state` is `active`, `revoked` or `expired`. The secret is never listed.

**Who may make one.** Any credential that may call `create_browser_key` and has write on the
account: a signed-in person, an API token scoped to it (or `*`), an agent's task credential, an
MCP client. `create_token` still refuses a token credential; a browser key does not need a session,
so it does not.

**What it binds.** `create_browser_key` reads the index (`get_index`) and the model (`get_model`)
from the workspace and refuses one that is not there (`not_found`). It does not require them to be
published: `index_published` and `model_published` say whether they are, and `search_index` and
`predict` refuse an unpublished one when called, as they always do. Origins are kept lowercase,
without a trailing slash and without duplicates; anything else is refused with `invalid_input` and
`path` `/origins/<n>`.

## Calling with a browser key

The key is a bearer token like any other:

```js
const url = new URL("https://lake.live/v1/search_index");
url.search = new URLSearchParams({customer_id, workspace_id, index_id, query}).toString();
const found = await fetch(url, {headers: {Authorization: `Bearer ${KEY}`}}).then((r) => r.json());
```

The browser sends `Origin` itself. From curl, send the website's origin:
`-H "Origin: https://shop.example.com"`. Every call names the key's `customer_id`, `workspace_id`
and `index_id` (or `model_id`); another value is refused with `unauthorized` and `path` at the
field. A call from an origin the key does not list, or with no `Origin`, is refused with
`unauthorized`, and its answer carries no `Access-Control-Allow-Origin`.

A preflight (`OPTIONS`) carries no credential, so Lake cannot tell which key will follow: it is
answered as every preflight is. The answer to the call itself is where CORS is narrowed.

A revoked key, an unknown one, or one whose workspace was deleted is `401 unauthenticated` with
`reason: "invalid"`; an expired one, `reason: "expired"`.

## Where it lives

Credentials are multitenancy (the keystore beside sessions and tokens), so a browser
key is a row of lake's PostgreSQL control plane, never in a workspace Zetta: table `browser_keys`
(migration `0102_browser_keys`, guard `browser_keys_guard`). The row holds the secret's SHA-256,
the binding (account, workspace, index, model), the origins, the calls a minute and the current
minute's count, who made it, its expiry, its revocation and when it was last used
([schema.md](../control-plane/schema.md#browser-keys-migration-0102_browser_keys)). The searches it
makes are recorded where every search is: usage in the workspace Zetta, audit in the workspace's
chain. Making, listing and revoking keys is audited in the central log, as `create_token` is.

The rate limit is the row: one `UPDATE` per call counts it, so every replica shares the same
minute. Resolving the key reads the row and writes `last_used_at`, as a session's use does.

## The quickstart

`quickstart_snippets` ([quickstart.md](quickstart.md#the-website-key)) names
`create_browser_key` bound to the quickstart's index (and its model once published) in `key.create`.
Called with `origins`, it makes the key through a nested `create_browser_key` call as the same
caller, and answers the secret once in `key.secret` and already in the code. The console does that
in one click with the website's address.

## Console

- **Quickstart**, step 3: type the website's address, *Make a website key*; the key and the code
  with the key in it are shown once ([../ui/quickstart.md](../ui/quickstart.md)).
- **Ship → Search Index**, *Website Keys*: make a key for this index for one or more addresses,
  see each key's websites, state and last use, and revoke it ([../ui/browser-keys.md](../ui/browser-keys.md)).

## Verify

- `ctest --test-dir build -R BrowserKeys` runs `Stores/BrowserKeys.*` (`tests/test_browser_keys.cpp`)
  over the in-memory store and, with `LAKE_TEST_DB_URI`, PostgreSQL: the binding refusals (another
  index, model, workspace, account or capability), origin refusals, a key that outlives its maker's
  sign-in and the token made under it, revocation, creation by a scoped API token and refusal for a
  read-only person, the calls a minute, metering to the workspace, a deleted workspace's key, and
  (PostgreSQL) expiry and the table's checks.
- `HttpResponseIsolation.ABrowserKeyAnswersOnlyItsWebsiteAndOutlivesSignOut`
  (`tests/test_http_server.cpp`) does it over HTTP: the CORS header for the key's origin and none
  for another, the preflight, sign-out, and revocation.
- `Quickstart.ADroppedDatasetBecomesSearchThenModelAnswers` makes the key through
  `quickstart_snippets` with `origins`.
- By hand: `create_browser_key` over MCP with an API token, then
  a `GET` of `search_index` with `Authorization: Bearer lbk.…` and `Origin: https://shop.example.com`
  answers, and the same with `Origin: https://other.example.com` answers 403.

## How to change it

- Another capability a key may call: `authorize_browser_key` in `src/api/registry.cpp` and
  `browser_key_principal` in `src/control/browser_keys.cpp`, its binding a column of `browser_keys`
  in a new migration, and this page.
- The origin rule: `browser_key_origin` in `src/control/browser_keys.cpp` (and `siteOrigin` in
  `frontend/src/pages/quickstart/quickstart.ts`, which only says early what Lake will refuse).
- The default and largest calls a minute: `browser_key_default_rate_limit` and
  `browser_key_max_rate_limit` in `include/lake/control/store.hpp`, and the table's check.
