# Application authentication API

## What

Lake applications use the identity core as a service. Registering an application creates an
isolated `pool_id`, an `application` policy object beneath its customer, and an Ed25519 signing
key. Password, TOTP, recovery-code, passkey, OAuth, and organization SSO capabilities already
take a pool and use the same credential, device, session, and identity-document tables as lake's
own `lake` pool. A credential from one pool is never considered by another pool.

The registration records a display name, exact allowed origins, redirect URIs, an optional custom
sign-in host, a JSON theme, and the **workspace** of the account the application belongs to
(`workspace_id`, required and never changed). That workspace's Zetta holds the application's
settings, the name, origins, redirect URIs and theme ([Where settings live](#where-settings-live-lake351)),
its users and organizations ([Where users and organizations live](#where-users-and-organizations-live-lake353-lake354)),
its signing keys and webhooks ([Where keys and webhooks live](#where-keys-and-webhooks-live-lake352-lake355)),
and its lifecycle events and webhook delivery attempts ([Lifecycle webhooks](#lifecycle-webhooks)). Browser requests carrying an `Origin` header must come from the
hosted page itself or an allowed origin. Redirects use only a registered URI. The custom host is a
DNS and ingress concern: route that host to lake with a valid certificate, then lake selects the
application by `Host`.

## Capabilities

All protected capabilities resolve the customer or `application` policy object and pass through
`authorize`. `Registry::call` appends the signed audit event with `audit::record` for success,
denial, and failure.

| Capability | Authorization | Result |
|---|---|---|
| `register_application` | customer `admin` | application, including its new `pool_id` and its `workspace_id` |
| `list_applications`, `get_application` | customer/application `read` | registrations with their settings read from each application's workspace, without key material; a listed application whose settings cannot be read carries `settings_unavailable` with the reason |
| `update_application_theme` | application `admin`; `workspace_id` must be the application's | updated registration |
| `rotate_application_signing_key` | application `admin`; `workspace_id` must be the application's | new key ID and version |
| `application_jwks` | public; by `application_id` alone | all retained public verification keys |
| `issue_application_jwt` | application `write`; `workspace_id` must be the application's | short-lived user access token |
| `begin_application_authorization` | application `write` | one-use code, state, and exact registered redirect for the signed-in lake user; the code is issued into the application's workspace |
| `exchange_application_authorization_code` | public client; by `application_id` and the code | EdDSA access and ID tokens after exact redirect and PKCE S256 verification; the code is redeemed once, in the application's workspace |
| `list_application_users`, `export_application_user` | application `read`; `workspace_id` must be the application's | pool user profiles and status, newest first |
| `suspend_application_user`, `delete_application_user` | application `admin`; `workspace_id` must be the application's | suspended profile or deletion confirmation |
| `list_application_organizations` | application `read`; `workspace_id` must be the application's | application-owned organizations, newest first |
| `create_application_organization`, `update_application_organization`, `delete_application_organization` | application `admin`; `workspace_id` must be the application's | organization lifecycle result; a TAAP identity or name the application already has is `conflict` |
| `create_application_webhook` | application `admin`; `workspace_id` must be the application's | webhook registration and its secret, returned once |
| `list_application_webhooks` | application `read`; `workspace_id` must be the application's | secret-free registrations, newest first |
| `list_application_webhook_events` | application `read`; `workspace_id` must be the application's | events, newest first, each with its delivery attempts |
| `retry_application_webhook` | application `admin`; `workspace_id` must be the application's | the last appended attempt |

The existing public identity capabilities accept the application's `pool_id`. A successful
application sign-up or sign-in adds `access_token`, `token_type: Bearer`, and `expires_in` to the
ordinary session result. API-token issuance through `create_token` returns only the derived
session bearer, bounded by issuer scopes, expiry and ancestry; it does not also mint an independent
application JWT. Its `session.started` lifecycle event is retained. The derivation policy and REST/MCP
PostgreSQL coverage are in [identity.md](identity.md#token-derivation-and-revocation). Suspending a user revokes that user's sessions in this pool immediately;
deleting one removes this pool's credentials and sessions. Neither action touches another pool.

### Where settings live (lake#351)

An application's settings, its display name, allowed origins, redirect URIs and hosted-page theme,
are one record in the workspace Zetta of the application's workspace: the sealed collection
`lake_auth_applications`, keyed by the application id and found by its account (`customer:<id>`)
([workspace-records.md](../architecture/workspace-records.md)). Nothing falls back to PostgreSQL,
to another workspace or to the lake Zetta.

- **What stays central.** `auth_applications` keeps only the routing that finds an application
  before any workspace is read: `id`; `customer_id` (its account, and the parent of its policy
  object); `pool_id` (`app:<id>`, which the pool's sessions, credentials and tokens name);
  `custom_sign_in_domain` (the `Host` a custom-domain page is requested on, unique across lake, and
  the issuer host of its tokens); `workspace_id` (`0049`); `created_at` and `updated_at`. None of
  these is the application's configuration body. A new application's row leaves `name`,
  `allowed_origins`, `redirect_uris` and `theme` NULL (migration `0059`); a row registered before
  held them until the operator's copy, and migration `0070_drop_moved_tables` drops the four
  columns (lake#311).
- **Registration.** `register_application` writes the settings into the chosen workspace with
  `if_absent`, beside the first key ring, before the routing row; a registration refused afterwards
  (a custom host already taken) takes both records back out.
- **Reads.** `get_application` reads the routing row and then one `get_record` of the settings.
  `list_applications` reads the routing rows, then one `list_records` per workspace that holds any
  of the readable applications; an application whose settings cannot be read (its workspace not
  ready, no workspace, not yet copied) is still listed by its routing with `settings_unavailable`.
- **Sign-in.** The hosted pages read the settings to render the name and theme and to choose the
  redirect URI; the custom-domain pages find the application by `Host` first. A cross-origin
  identity call (`Origin` not the request's own) checks the allowed origins in the settings, and a
  passkey ceremony reads them only when the origin is neither lake's host nor the custom host;
  `begin_application_authorization` checks the redirect URI against them. Same-origin hosted
  sign-ins read nothing extra. `begin_passkey_registration`, `begin_passkey_sign_in` and
  `start_oauth` are asynchronous for this.
- **Theme.** `update_application_theme` takes the application's `workspace_id` and writes the new
  theme conditionally on the revision it read (`if_revision`); a concurrent change is reread and
  applied again, never lost. The name, origins and redirect URIs are set at registration.
- **Not ready, deletion.** A workspace that is not ready answers `workspace_not_ready`; a legacy
  application without a workspace the same. Deleting the workspace deletes the settings with its
  Zetta, and the application then answers an error instead of reading anything elsewhere.
- **Before the copy.** An application registered before this change had its settings only in its
  retired columns until the operator's copy ran: its pages and `get_application` answered
  `not_found`, and its cross-origin sign-ins were refused (cutover, done on dev before migration
  `0070_drop_moved_tables` dropped the columns:
  [workspace-records.md](../architecture/workspace-records.md#cutover-runbook-application-settings-dev-cluster)).

### Where users and organizations live (lake#353, lake#354)

An application's users and organizations are records in the workspace Zetta of the application's
workspace: the sealed collections `lake_auth_app_users` and `lake_auth_app_organizations`
([workspace-records.md](../architecture/workspace-records.md)). Lake's PostgreSQL keeps neither,
and nothing falls back to it, to another workspace or to the lake Zetta. What stays in PostgreSQL
is lake's own account of the person (the `users` row, the pool's credentials and sessions), which
`identity_documents`, `credentials` and `sessions` move separately (lake#320, lake#322, lake#323).

- **A user record** holds the application, the account id, its TAAP identity, email, display name,
  `status` (`active` or `suspended`) and whether it signed up in the application's own pool. It is
  keyed by the blind index of `application, user`, so a lake user who authorized two applications of
  one workspace has one record in each. Listing an application's users is one `list_records`; a
  search reads them all and filters email and display name, as the `ILIKE` did.
- **Unique within an application.** A pool user's email (compared in lowercase, as the pool's
  sign-in compares it) and an organization's TAAP identity and name are each a claim record in the
  same collection, keyed by the blind index of the value and written with `if_absent`. The claim
  names its holder, so two sign-ups with one email, or two organizations with one name, cannot both
  succeed: the second is `conflict`. A claim younger than two minutes always holds, because its
  sign-up may still be in flight; an older one whose holder does not exist (an account lake never
  created, an organization with no record) was abandoned and is taken over with `if_revision`.
- **Sign-up.** A password sign-up in an application's pool claims the email for an account id chosen
  first, then creates the account with that id in lake's store, then writes the user record. A
  failed account creation releases the claim. If lake stops after the account exists but before the
  record is written, the next sign-in completes it, because the claim names the account. A first
  OAuth sign-in in the pool creates the account in lake's store and then claims its email (when it
  has one) and writes the record.
- **Every session issued in an application's pool is checked there.** Password, TOTP, passkey,
  TAAP, OAuth, recovery and token sessions in an application's pool are returned only after the
  user's record is read in the application's workspace and is `active`; otherwise the new session
  is revoked and the caller receives `unauthorized` (`application user is suspended`, or the
  account is not a user of the application), or `workspace_not_ready`. Resolving a session on a
  later request no longer reads the user's status: suspension writes the status first and then
  revokes the pool's sessions, and a sign-in reads the status after its session exists, so a
  session issued while a suspension lands is revoked by one or the other. A suspended user with TOTP
  enrolled is told `mfa_required` and refused at the TOTP step, where the suspended check used to
  come first.
- **Asynchronous callers.** The sign-in, sign-up, MFA, passkey, recovery, SSO and token
  capabilities, and every user and organization capability, are asynchronous: they await the
  workspace instead of blocking a runtime thread (`block_on`), as lake#341 did for push. REST, MCP
  and the hosted pages call them through `Registry::call_async`; a synchronous `Registry::call`
  answers `capability requires asynchronous dispatch`.
- **Reading and writing.** The user and organization capabilities take the application's
  `workspace_id`. A missing, malformed, unknown, foreign or other workspace of the account is
  `invalid_input` at `/workspace_id`; the pages pass the application's own. An organization
  rename claims the new name first, writes the organization over the revision it read and then
  releases the old name; a concurrent change is `conflict` and releases the new claim.
- **Workspace not ready, or deleted.** A workspace that is not ready refuses every read, write and
  sign-in of its applications with `workspace_not_ready`. Deleting a workspace deletes its Zetta
  with the users and organizations of its applications, whose sign-ins then fail.
- **Cutover.** Users from before the move were in the workspace only once the operator's one-shot
  copy (`copy_workspace_records`, migration `0055_application_user_workspaces`) had run; until then
  their sign-ins were refused as not users of the application. The copy was verified on dev, and
  migration `0070_drop_moved_tables` drops `auth_app_users` and `auth_app_organizations`. The runbook is in
  [workspace-records.md](../architecture/workspace-records.md#cutover-runbook-application-users-and-organizations-dev-cluster).

## Hosted pages and HTTP routes

| Route | Purpose |
|---|---|
| the web app's `/applications`, `/application?id=<id>`, `/application/users`, `/application/organizations`, `/application/events` | registration and management, through the capabilities above (the server forms were retired in lake#561) |
| `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed hosted identity pages on lake's host |
| the same `/sign-up`, `/sign-in`, and `/mfa` paths on the custom host | themed custom-domain entry pages |
| `GET /v1/apps/<id>/.well-known/jwks.json` | raw public JWKS, cached for five minutes |

The form adapters call the registered identity capabilities with the application pool. Sign-up,
password sign-in, and TOTP verification set the same HttpOnly session cookie as lake identity.
When a redirect URI is configured, successful password, MFA, passkey, and OAuth authentication
returns `access_token`, `token_type`, and `expires_in` in its URI fragment; fragments are not sent
to the redirect server and the application reads and clears them in its own page. Lake never
redirects to a URI outside the registration.
The hosted passkey page contains the sole application-page script: the documented
`navigator.credentials.get` WebAuthn call and its JSON posts. The other application identity
pages contain no script. Theme colors accept hexadecimal CSS colors; all application values are
HTML-escaped.

## JWT contract and verification

Tokens use `alg: EdDSA` and a `kid` present in the application's JWKS. Claims include `iss`, the
user TAAP identity as `sub` (or the user UUID if no TAAP identity exists), the application UUID as
`aud`, `application_id`, `pool_id`, `user_id`, `iat`, `exp`, and `jti`. The default lifetime is 15
minutes and cannot exceed one hour. A rotation makes a new key current but retains retired public
keys so already-issued tokens continue to verify.

The OIDC code path uses `begin_application_authorization` after lake session resolution. It
registers the lake user in the application, requires an exact configured redirect, accepts only
PKCE S256, and persists only the code hash with a five-minute expiry, in the application's
workspace ([Where authorization codes live](#where-authorization-codes-live-lake363)). Exchange
consumes the code before checking its redirect and verifier, so every completion attempt is
one-use. The ID token
adds the request nonce and uses `https://<application host>/v1/apps/<id>` as issuer, the application
UUID as audience, and the same keys published by `application_jwks` and the raw JWKS route.

Always require EdDSA, fetch keys from the exact application JWKS URL, and compare `aud` to the
application UUID. Common runtime libraries perform the key selection from `kid`:

```js
// Node.js with jose
const jwks = createRemoteJWKSet(new URL(jwksUrl))
const { payload } = await jwtVerify(token, jwks, {
  algorithms: ['EdDSA'],
  audience: applicationId,
  issuer
})
```

```python
# Python with PyJWT
key = jwt.PyJWKClient(jwks_url).get_signing_key_from_jwt(token)
claims = jwt.decode(token, key.key, algorithms=["EdDSA"],
                    audience=application_id, issuer=issuer)
```

```go
// Go with github.com/golang-jwt/jwt/v5; keyFunc selects kid from the fetched JWKS.
parsed, err := jwt.Parse(token, keyFunc,
    jwt.WithValidMethods([]string{"EdDSA"}),
    jwt.WithAudience(applicationID), jwt.WithIssuer(issuer))
```

Rust applications can use `jsonwebtoken` with an EdDSA decoding key selected from the JWKS and
set `Validation.algorithms`, `Validation.aud`, and `Validation.iss` before decoding.

### Where authorization codes live (lake#363)

An application's one-use OIDC authorization codes live in the workspace Zetta of the application's
workspace: the sealed collection `lake_auth_app_authorization_codes`, found by the application
(`application:<id>`) ([workspace-records.md](../architecture/workspace-records.md)). A code's
record key is the blind index of the application and the code's SHA-256, so the workspace holds
neither the code nor its hash in the clear; the sealed body holds the user, the redirect URI, the
PKCE challenge, the nonce and the expiry. Nothing falls back to PostgreSQL, to another workspace or
to the lake Zetta. (`app_authorization_codes`, lake's own protected-app forward-auth handoff, is a
different table: it is multitenancy and stays central.)

- **Issue.** `begin_application_authorization` writes the code into the application's workspace
  with `if_absent`. The workspace is the application's (`auth_applications.workspace_id`, `0049`),
  so the capability takes no `workspace_id`.
- **Exchange.** The relying party's backend calls `exchange_application_authorization_code` with the
  application id and the code only. Lake reads the application's routing row for its workspace,
  reads the code there and deletes it at the revision it read (`delete_record` with
  `if_revision`). Of two exchanges racing for one code only one delete succeeds; the other gets
  `conflict` or `not_found` and is refused with `unauthorized`, so a code is redeemed at most once.
  The redirect and PKCE verifier are checked after the code is consumed, as before.
- **Expiry.** The collection's retention is the code lifetime, five minutes after the write, and
  the engine hides and removes an expired record. Lake also refuses a code whose own expiry has
  passed (and removes it), which holds for a copied code that was written after it was issued.
- **Refusals.** A code that is absent, of another application, expired or already redeemed is
  `unauthorized`. A missing `application_id` is `invalid_input` at `/application_id`; an unknown or
  malformed one is refused at `/application_id`. An application without a workspace, or whose
  workspace is not ready, answers `workspace_not_ready`; a deleted workspace takes its codes with
  it.
- **Before the copy.** A code issued in the five minutes before the deploy was in the retired table
  until the operator's copy wrote it into the workspace; until then its exchange was refused
  (cutover, done on dev before migration `0070_drop_moved_tables` dropped the table:
  [workspace-records.md](../architecture/workspace-records.md#cutover-runbook-application-authorization-codes-dev-cluster)).

## Lifecycle webhooks

Supported event names are `user.created`, `user.updated`, `user.deleted`,
`organization.created`, `organization.updated`, `organization.deleted`, `session.started`, and
`mfa.changed`. The JSON body is:

```json
{"id":"event UUID","application_id":"application UUID","event":"session.started","created_at":"timestamp","data":{}}
```

Lake sends `Content-Type`, `X-Lake-Event`, `X-Lake-Event-Id`, and `X-Lake-Signature`. The signature
is lowercase hex `v1=HMAC-SHA256(secret, event_id + "." + exact_body)`. Compare it in constant
time before parsing or acting on the event. A non-2xx response or transport failure is retained
and retried up to three consecutive attempts. `retry_application_webhook` appends another bounded
set without changing earlier attempts. The event log retains status, signature, error, and attempt
number for each delivery.

### Where events and attempts live (lake#356, lake#357)

An application's events and delivery attempts are records in the workspace Zetta of the
application's workspace: the sealed collections `lake_auth_app_webhook_events` and
`lake_auth_app_webhook_deliveries` ([workspace-records.md](../architecture/workspace-records.md)).
Lake's PostgreSQL holds no event payload, signature or outcome, and nothing falls back to it, to
another workspace or to the lake Zetta.

- **Writing.** Every lifecycle event (sign-up, sign-in, MFA changes, user and organization
  changes) is appended to the application's workspace before the caller is answered, as it was
  written to the control store before. An application with no workspace yet (a legacy
  registration whose account had several workspaces) answers `workspace_not_ready`, and so does a
  workspace that is not ready. The event is then not recorded and the action that raised it fails.
- **Exactly one sender per attempt.** Each attempt is a record keyed by the blind index of
  `event, webhook, attempt`. Lake claims it with `if_absent` and a `pending` outcome before it
  sends, and records the status with `if_revision` afterwards. A caller whose claim conflicts
  sends nothing, so two lake processes, or two concurrent retries, never send the same attempt. A
  retry while the latest attempt of that event to that webhook is still `pending` answers
  `conflict`. A pending claim older than five minutes was abandoned by a caller that stopped (an
  outbound exchange times out after 30 seconds), and a retry continues after it.
- **There is no background retry loop.** Delivery is the three inline attempts plus
  `retry_application_webhook`, as before, so lake keeps no due-delivery index anywhere.
- **Reading.** `list_application_webhook_events` and `retry_application_webhook` take the
  application's `workspace_id`. A missing, malformed, unknown, foreign or other workspace of the
  account is `invalid_input` at `/workspace_id`; the page passes the application's own. A listing is
  two record reads: the newest events, then the attempts back to the oldest of them.
- **Retention and deletion.** Events and attempts are kept until their workspace is deleted, which
  deletes its Zetta and its records. The application's events then can no longer be written, so
  its sign-ins fail until the application is registered again in a live workspace.

### Where keys and webhooks live (lake#352, lake#355)

An application's signing keys and its webhooks, with their signing secrets, are records in the
workspace Zetta of the application's workspace: the sealed collections
`lake_auth_app_signing_keys` and `lake_auth_app_webhooks`
([workspace-records.md](../architecture/workspace-records.md)). Lake's PostgreSQL holds no key,
public key, webhook URL or secret, and nothing falls back to it, to another workspace or to the
lake Zetta.

- **Credentials stay enveloped.** A signing seed or webhook secret is first protected by the
  secrets subsystem's envelope: AES-256-GCM under a data key for the purpose
  `application.jwt.<application id>` or `application.webhook.<webhook id>`, wrapped by
  `LAKE_INSTANCE_KEK` in the `keystore` table (`Store::encrypt_application_credential`). That
  envelope is kept only inside the record's sealed body, which the workspace's own record key seals
  again. The value is opened only to sign a token or a delivery, and never appears in list,
  application, event, or audit output. The keystore rows are wrapped data keys, not credentials;
  they move with the keystore (lake#324).
- **Keys.** One record per application, keyed by the application id, holds every key in version
  order with exactly one current: the key ring. `register_application` writes it into the chosen
  workspace with `if_absent` before the application row exists, so no application exists without
  a key (a registration refused afterwards takes the ring back out).
  `rotate_application_signing_key` reads the ring, retires the current key and adds the next one in
  **one write conditional on the revision it read** (`if_revision`). A concurrent rotation makes
  that write `conflict`; lake rereads and makes the next version, so a rotation is never lost and
  there is never more than one current key. Retired public keys stay in the JWKS.
- **The JWKS** is read anonymously by relying parties that know only the application id. Lake reads
  the application's routing pointer (`auth_applications.workspace_id`) and then one `get_record`
  of the key ring; it opens no private key. The raw route keeps its five-minute cache header.
  Sign-up, sign-in and the code exchange find the workspace the same way, since their protocols
  carry no workspace; `issue_application_jwt` takes the application's `workspace_id`. Every issuer
  reads the key on its asynchronous path.
- **Webhooks.** One record per webhook, keyed by its id and written once with `if_absent`. Each
  lifecycle event lists the application's webhooks in the same workspace (one `list_records`) and
  opens the secret of each subscribed webhook to sign its delivery. `create_application_webhook`,
  `list_application_webhooks` and `rotate_application_signing_key` take the application's
  `workspace_id`; a missing, malformed, unknown, foreign or other workspace of the account is
  `invalid_input` at `/workspace_id`. The application page passes the application's own.
- **Not ready, or no workspace.** A workspace that is not ready, or a legacy application with none,
  answers `workspace_not_ready`: no token is issued, no JWKS is served and no webhook is created.
- **Deletion.** Deleting the workspace deletes the application's keys and webhooks with its Zetta;
  its JWKS and sign-ins then fail rather than read anything elsewhere.
- **Before the copy.** An application registered before this change had its keys and webhooks only
  in the retired tables until the operator's copy ran, so its sign-ins failed and its JWKS was empty
  until then (cutover, done on dev before migration `0070_drop_moved_tables` dropped the tables:
  [workspace-records.md](../architecture/workspace-records.md#cutover-runbook-application-keys-and-webhooks-dev-cluster)).

## ApplicationAgent and Ship Apps

ApplicationAgent builds customer-hosted **Ship Apps**; it does not register the identity
applications described above. It is the `agent-application` Agent Library service, runs on the
agent bus topic `agent-tasks.agent-application`, and authenticates its bus calls with the
`lake-<name>-agent-bus` Secret key for that agent.

An initial `agent_turn` has kind `app` and only read capabilities: workspace dataset discovery and
read-only SQL, Service and App reads, and repository reads. Its answer contract is
`{reply, result: {kind, files, app, confirm}}`. When `kind` is `proposal` and `confirm` is true, a
person reviews the files and App settings. Lake starts a later turn with that exact object at
`context.confirmed_proposal`; only the later turn receives `create_repository`,
`commit_repository_files`, `create_app`, and `deploy_app`.

The confirming call names what it confirms, and `agent_turn` checks it against the stored answer
rather than trusting the caller's copy (lake#561):

```json
{"customer_id":"account-id","workspace_id":"workspace-id","agent":"agent-application","kind":"app","message":"Confirm and deploy the reviewed proposal.","proposal_task_id":"task-id","proposal_sha256":"<proposal_sha256 from get_agent_task>","context":{"confirmed_proposal":{"kind":"proposal","confirm":true,"files":[],"app":{"name":"customer-health","auth":"none"}},"confirm_public":true}}
```

`proposal_task_id` must be a finished `agent-application` `app` turn of this workspace that the
caller may follow, and its answer a proposal with `confirm: true`, `files` and `app`
(`That task has no ApplicationAgent proposal to confirm.`, 400). `proposal_sha256` must be the
digest of that stored proposal, which `get_agent_task` returns beside it, and `confirmed_proposal`
must equal it (`The ApplicationAgent proposal changed before confirmation.`, 409). A public app
(`auth` `none` or `public`) also needs `context.confirm_public: true` (`This app will be public.
Tick the box to confirm that anyone with its address can open it.`, 400).

ApplicationAgent's App shape maps `auth: public` to Ship App `auth: none`, accepts a missing or null
host as the generated wildcard host, and represents an operator-managed environment credential as
`{"NAME":{"secret":"reference"}}`. `deploy_app` accepts the service's `app` key as an alias for
`app_id`. It must propose a non-reserved App name: Lake protects the platform names and prefixes
listed in [web-apps.md](web-apps.md), plus the operator's `LAKE_APPS_RESERVED_NAMES`, and reports a
refusal at `/name`. The full desired-state contract is [web-apps.md](web-apps.md), and the review
page is [the Ship Apps UI](../ui/apps.md).

## How to verify

```sh
cmake --build build
LAKE_TEST_DB_URI=postgresql://lake:password@127.0.0.1:5432/lake_test \
  ctest --test-dir build --output-on-failure \
    -R 'ApplicationAuth|ApplicationHttp|PostgresApplications|PostgresApplicationUsers|PostgresApplicationSettings|PostgresApplicationAuthorizationCodes|TheOneShotCopyMovesApplication|TheOneShotCopyMovesLiveApplicationAuthorizationCodes|IdentityExit'
```

`ApplicationAuth` verifies a real Ed25519 JWT against the returned JWKS and checks its audience,
records a failed webhook delivery followed by its successful retry, suspends the user, and proves
pool isolation. Its policy case proves application lists filter inaccessible registrations and
reject access to another application's user list. `ApplicationHttp` submits the hosted forms,
renders every custom-domain entry page, and reads the raw JWKS route.
`ApplicationAuth.EventsAndDeliveriesLiveInTheApplicationsWorkspace` covers two accounts, two
workspaces in one account, missing, malformed, unknown and foreign `workspace_id`, a reader who may
list but not retry, a workspace that is not ready, and workspace deletion.
`ApplicationAuth.AnAttemptIsClaimedOnceAndSentOnce` proves a claimed attempt is not sent by a
second caller and that an abandoned claim does not block retries.
`ApplicationAuth.UsersAndOrganizationsLiveInTheApplicationsWorkspace` covers users and
organizations in two accounts and two workspaces of one account: sealed records and claims only at the
application's own endpoint, a second sign-up with the same email (in any case) and a second
organization with the same TAAP identity or name refused, a fresh claim holding and an abandoned one
taken over, a sign-up completed at the next sign-in, an account that is not a user refused with no
live session left, suspension, deletion freeing the email, renames and deletions freeing names,
missing, malformed, unknown and foreign `workspace_id`, least privilege, a workspace that is not
ready, and workspace deletion. `WorkspaceRecordStores.TheOneShotCopyMovesApplicationUsersAndOrganizations`
covers their copy in both stores and `PostgresApplicationUsers.MigrationRecordsEachRowsOwnerForTheCopy`
migration `0055`.
`ApplicationAuth.KeysAndWebhooksLiveInTheApplicationsWorkspace` covers keys and webhooks in two
accounts with two workspaces each: sealed records only at the application's endpoint and none in the
control store, the anonymous JWKS by application id, tokens verifying with the application's key
only, missing, malformed, unknown and foreign `workspace_id` for every caller, deliveries signed
with the opened secret, rotation, least privilege, a workspace that is not ready, and deletion.
`ApplicationAuth.AConcurrentRotationIsRereadNotLost` races two rotations and a stale write.
`ApplicationAuth.SettingsLiveInTheApplicationsWorkspace` covers settings in two accounts with two
workspaces each: one sealed record only at the application's endpoint and none in the routing row,
reads and listings filled from the workspace, the theme changed with missing, malformed, unknown and
foreign `workspace_id` refused and a raced change reread, a cross-origin sign-in allowed only from
the recorded origins, least privilege, a record moved under another application's key not
opening, a refused registration leaving nothing behind, a workspace that is not ready (listed with
`settings_unavailable`), and deletion.
`WorkspaceRecordStores.TheOneShotCopyMovesApplicationSettings` covers their copy in both stores and
`PostgresApplicationSettings.MigrationRecordsEachRowsOwnerForTheCopy` migration `0059`.
`PostgresApplications` exercises the registry capabilities against migration `0016_auth_apps` and
the routing trigger of `0049`. `PostgresApplicationWebhooks.MigrationRecordsEachEventsOwnerForTheCopy`
covers migration `0049`, and `WorkspaceRecordStores.TheOneShotCopyMovesApplicationEventsAndTheirDeliveries`
the copy in both stores. `PostgresApplicationCredentials.MigrationRecordsEachRowsOwnerForTheCopy`
covers migration `0056`, and `WorkspaceRecordStores.TheOneShotCopyMovesApplicationKeysAndWebhooks`
the key and webhook copy in both stores, including signing with copied keys and secrets.
`ApplicationAuth.AuthorizationCodesLiveInTheApplicationsWorkspace` covers codes in two accounts with
two workspaces each: one sealed record under a blind key only at the application's endpoint and the
collection's retention, an exchange through another application refused, one redemption and a
replay refused, two racing exchanges (the other's delete first, and a stale revision) refused, an
expired code refused and removed, `if_absent` issue, a record moved under another application's key
not opening, missing, malformed and unknown `application_id`, least privilege, a workspace that is
not ready, and deletion. `WorkspaceRecordStores.TheOneShotCopyMovesLiveApplicationAuthorizationCodes`
covers the copy of live codes in both stores, and
`PostgresApplicationAuthorizationCodes.MigrationRecordsEachLiveCodesOwnerForTheCopy` migration `0061`.
`IdentityExit.PostgresProvesIdentityPkiAndDynamicSecrets` starts from a liquid TAAP-key lake
session, completes the PKCE code exchange as a test relying party, and validates the ID token
signature and claims against the application's JWKS.

## How to change

Keep pool authentication in the identity core. Add application records through the `Store`
interface and both store implementations, expose behavior as a registry capability with an
application policy-object resolver, and let pages call that capability. Add a lifecycle event only
with its exact payload, webhook test, audit outcome, and this contract updated together.
