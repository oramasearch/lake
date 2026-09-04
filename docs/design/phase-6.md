# Phase 6 design: lake

> Plan: [`action-plan.md`](https://github.com/oramasearch/liquid-bridge/blob/main/docs/action-plan.md),
> epic https://github.com/oramasearch/lake/issues/11. Decisions that shape this page (bridge
> `docs/decisions.md`): §6 planes (Postgres control plane, Zetta data plane), §10 Phase 6 starts
> now in parallel, §11 auth as a service in scope, §12 clean secrets API, §13 SOC 2.
> Specification: liquid `docs/features/18-lake-service.md`, `19-identity.md`,
> `20-secrets-service-and-pki.md`, `25-identity-attestation-and-transparency.md`, `12-search.md`,
> `15-consumer-sync-and-web.md`. This page is written before the issues and corrected by them.

lake today is the control surface: one capability registry rendered as REST `/v1`, OpenAPI and
MCP; users, organizations, customers and repositories in Postgres; server-rendered pages,
including anonymous reads of public repository data; telemetry; CI. Organization administration and the authorization object graph
described below are implemented by lake issues #16 and #19 in migration
`0004_organizations_policy`; identity core is implemented in migration `0007_identity_core`.
Everything
else below is added to that shape and keeps its rules
(`CLAUDE.md`: one adapter TU, one registry, one auth check, trailing 404, pages never do what
the API cannot).

**Web UI status: repository browsing is implemented in lake #9.** Repository lists, refs,
trees, files, structural change diffs, conflict values, organization and team access views render without
client-side code. Every returned repository candidate passes batched policy authorization, and
anonymous requests can read only public repositories. Artifact records and attestations remain
deferred while lake integrates liquid #166's public result APIs.

Identity-core status on this branch: verified TAAP documents, password plus TOTP with one-use
recovery codes and fresh-verification disablement, persisted
WebAuthn, OAuth, row-resolved sessions, scoped tokens and device revocation are implemented. The
hosted sign-up, sign-in, MFA, passkey, sessions/devices, and token pages use those capabilities.
The signed audit substrate from #18 is integrated, and every passkey registration stores a
lake-signed TAAP binding beside the credential. Application user pools remain owned by #51.

**SSO federation status (#15, #21).** Organization SAML and OIDC provider records, encrypted
OIDC client secrets, hashed SCIM tokens, federation issuer-claim policy, JIT user mapping,
SCIM user/group provisioning, session revocation on SCIM deactivation, and provisioning logs are
implemented in `0010_sso_federation`. Provider configuration, testing, federation policy,
provisioning, and SSO sign-in are registry capabilities and append signed audit events. Their
contract is documented in [`../api/sso.md`](../api/sso.md).

## Identity: one substrate, three principals

Every principal is a TAAP identity (liquid `docs/library/identity.md`: Ed25519 keys, SHA-512/256
fingerprints, self-signed identity documents, dual-signed rotation). lake stores the identity
document and its rotation chain for every user, workload and organization it knows; the
`taap_id` already on `users` and `organizations` is the durable key. Credentials are ways to
prove control of that identity, never the identity itself.

| Principal | Authenticated by | Stored as |
|---|---|---|
| Person | password with MFA, passkey (WebAuthn), OAuth social, enterprise SSO (SAML, OIDC), each bound to the person's TAAP identity | `users` plus `credentials` (one row per method), `sessions`, `devices` |
| Workload | short-lived credential issued to a TAAP key (a build worker, a deployed application, an agent session) | `workloads` plus `credentials` of kind `attested` |
| Organization | membership and roles asserted as TAAP attestations signed by an administrator's identity | `organizations`, `teams`, `memberships` carrying the attestation digest |

**Sessions and tokens.** A session is a row with a random secret, a device, an expiry and a
revocation flag; the cookie (`lake_session`) and the API bearer resolve through the one
`context_for` path, exactly as today. Per-user API tokens are sessions with a name, a scope and a
longer life, listed and revoked on a page and through capabilities; revocation takes effect on
the next request because every request resolves the row. Long-lived static tokens are an explicit,
audited exception (`kind = static`, reason required).

**Auth as a service (decision §11).** An application deployed on lake registers and receives a
*user pool*: the same tables scoped by `pool_id` (lake's own users are pool `lake`), the same
authentication methods, hosted sign-in pages on lake's domain or the application's, JWT issuance
per application audience with a JWKS endpoint, and lifecycle webhooks. Nothing in the identity
core is written twice; the application-facing surface is the follow-up epic (#51) and every
identity-core issue keeps `pool_id` on its tables and in its capability inputs from the start.

### Identity-core security decisions

- A person's TAAP private key never reaches lake. The client holds it. lake stores identity
  documents and rotation chains and verifies their signatures. A WebAuthn credential is the
  browser device key; lake stores its credential-id and public-key binding to the user's TAAP
  identity after an attestation-less registration, records the registration in the signed audit
  stream, and stores a `passkey_binding/1` TAAP attestation signed by lake's current instance
  identity. The binding remains verifiable through identity history after rotation. lake issues a
  session only after a successful assertion. lake #18 supplies lake's instance identity: it is
  generated on first start, its seed is wrapped by the instance KEK (or provided as
  `LAKE_IDENTITY_SEED`), and its public document is exposed at `GET /v1/identity`. Identity
  capabilities and the OAuth callback write through its signed audit stream.
  lake verifies identity documents through libliquid's public canonical-CBOR codec.
- TOTP seeds and OAuth client secrets are encrypted with AES-256-GCM under a
  purpose-specific data key. `keystore(purpose, wrapped_key, version)` holds those keys wrapped
  by the instance KEK; #24/#25 generalise the table and its lifecycle.
- Passwords use OpenSSL 3.2+ `EVP_KDF` `ARGON2ID`; lake's current build uses OpenSSL 3.6.3 with
  64 MiB memory, three iterations, and one lane. An older build must use the documented scrypt
  fallback until its OpenSSL is upgraded.
- OAuth providers are registered per pool with issuer, client id, encrypted client secret and
  scopes. The authorization-code flow uses PKCE and callback
  `https://<LAKE_PUBLIC_HOST>/v1/oauth/<provider>/callback`; Google id tokens are verified against
  cached issuer JWKS, while GitHub uses its authenticated user endpoint. The first providers are
  GitHub and Google. OAuth state and the encrypted PKCE verifier expire after five minutes and are
  consumed once.
- `LAKE_PUBLIC_HOST` is required for WebAuthn RP ID and OAuth callback construction. Each pool
  configures its allowed origins. Registration and assertion challenges live in
  `identity_webauthn_challenges`, expire after five minutes, and are consumed once. The accepted
  registration attestation format is `none`; assertions verify the stored public key and sign
  counter.

## Authorisation: one policy engine, one object graph

Repository, drive and platform permissions are three surfaces over one engine. The object graph
is: customer → organization → team → member; repository → path (a path is a node id from
liquid's tree, so a folder in the drive and a directory in a repository are the same object);
environment → secret mount → secret. A permission is `(principal, object, role)` where role is
one of a fixed set per object type (`read`, `write`, `admin`, `deploy`, `review`, `share`), stored
as an attestation-backed row in `grants`, inherited down the graph unless a node sets
`inherit = false`, and resolved by one function `authorize(principal, object, action)` that the
registry calls before any capability body and that pages call through the same code. The
resolution walks up the graph once and caches per request; it never consults a second system.

Query-time filtering for search and lists calls the same function per candidate object with a
batched walk, so a result set never shows an object the caller could not open (#40).

**Implemented status (#16, #19).** `teams`, `memberships`, `invitations`, `objects`, and `grants`
are control-plane tables. Membership and grant rows retain the TAAP attestation digest and the
granting administrator identity. `Registry::call` invokes `authorize` before protected handlers;
the server-rendered pages call those same capabilities. `authorize_batch` shares cached ancestor
and grant loads across candidates, and `effective_access` exposes direct and inherited access.
The current operator-admin principal is handled as a break-glass principal by the same function.
Identity-core credential resolution supplies user TAAP principals; self-management resolves
`principal/<taap_id>`, and scoped API tokens are narrowed before policy evaluation. Workload TAAP
principals remain part of the workload-identity follow-up.

## Audit (decision §13)

**Implemented status (#18).** The implementation creates the
append-only `audit_events` stream and all wave-one branches write this shape through
`audit::record`; a later wave may add event kinds but may not reinterpret an existing field.

One append-only `audit_events` stream records authentication (success and failure, method,
device), session and token issuance and revocation, every authorisation decision that denied,
every grant change, every secret and certificate issuance, access, renewal and revocation, and
every administrative action. The canonical signed payload is canonical CBOR with this versioned
shape:

| Field | Meaning |
|---|---|
| `version` | `audit_event/1`; a new canonical shape receives a new version. |
| `id`, `occurred_at` | UUID event identifier and UTC RFC 3339 timestamp with microsecond precision. |
| `customer_id` | Owning customer when an event is customer-scoped; null for lake-instance events. |
| `kind` | Stable event name such as `capability`, `authentication`, `session`, `token`, `authorisation`, `grant`, `secret`, `certificate`, `administrative`, or `retention`. |
| `principal` | The acting TAAP identity; `anonymous` is used before authentication identifies a principal. |
| `object` | Stable object identifier, including its type, or `lake` for instance-wide actions. |
| `action`, `outcome` | Attempted action and one of `success`, `failure`, `denied`, or `revoked`. |
| `request_id` | Opaque request correlation identifier, generated before registry dispatch. |
| `method`, `device` | Authentication method and device identifier when known; otherwise null. |
| `details` | Canonical JSON-compatible facts needed to interpret the event; secrets, credentials and private key material never appear here. |
| `previous_digest` | SHA-512/256 digest of the preceding stored event, or null for the first event. |
| `signer` | lake's TAAP identity. |
| `signature` | Base64 Ed25519 signature by `signer` over `TAAP/1 audit_event/1`, a zero byte, and the canonical CBOR payload. |
| `digest` | SHA-512/256 digest of the complete stored event excluding `digest`; it is the next event's `previous_digest`. |

`audit::record` appends exactly one signed event for each registry capability outcome, including
authorization and validation failures. Identity, policy, secrets and certificate code calls the
same function for events that do not originate in a registry call. The verifier recomputes each
signature and digest and checks each predecessor link, so a deleted or reordered row is detected.

The instance signing identity is a TAAP/1 Ed25519 identity. At first start lake uses
`LAKE_IDENTITY_SEED` directly when configured, without copying that Secret into the control
database; otherwise it creates an Ed25519 seed and persists it wrapped by the
customer-independent `LAKE_INSTANCE_KEK` in `lake_identity`. `GET /v1/identity` publishes the
identity document. `rotate_identity` creates and verifies the TAAP dual-signed rotation document,
appends it to the identity chain, and records its audit event. Rotation creates a wrapped generated
successor, so it requires `LAKE_INSTANCE_KEK` even when the current identity came from the
operator Secret.

The `audit` capability and its server-rendered page filter by principal, object, outcome and time.
`export_audit` returns the matching rows as JSON lines for evidence handling and round-trip
verification. Retention is configurable upward per customer with a non-negotiable 400-day floor;
an attempted deletion below the floor is refused and recorded as a `retention` event. SOC 2
evidence (access reviews, change management) is a query over this stream plus the forge's change
records; nothing is reconstructed after the fact.

## The data plane: customer liquid stores

The control plane (Postgres) knows *that* a repository exists, who may touch it and what its
heads are. The content lives in one liquid disk store per customer at
`LAKE_STORE_ROOT/<customer id>/store`. `LAKE_STORE_ROOT` is a read-write PersistentVolume mounted
by the operator (operator #132); lake does not provision or select that volume. liquid's store
code runs inside lake as the library-only `liquid::liquid` target, and its pack files, index runs,
manifest, and collector state remain libliquid-owned. The chunk index is liquid's own, held per
customer store. Zetta remains lake’s serving-model reference, not this repository-data backend.

**Push and pull (#12) is implemented** with liquid #160 and #165's published
`docs/library/sync.md`, public client, and `sync_server`. Lake registers `refs`, `find_missing`,
`push`, `pull`, chunk upload, and ranged chunk read through the capability registry. The adapter
opens every repository with `repository_options.store` against its customer store, delegates the
binary protocol to `sync_server`, authorizes each pushed change, and records all outcomes.

**Repository lifecycle (#13)** stays in the control plane: create, rename, transfer between
customers, delete (tombstone first, content collected by the customer store's collector after
the retention window), visibility (private, organization, public), each a capability and a page,
each an audit event.

## Search (#6, #41, #40)

Indexed by blob once (content-addressed: a blob is indexed the first time any repository stores
it), with oramacore-cpp's BM25F and positional trigram index as the engines, a hot index on branch
tips and cold snapshots for releases and tags, publication atomic per change (a query sees a
change entirely or not at all), and the policy engine filtering candidates at query time. The
shared framing is published in liquid `docs/library/sync.md` and mirrored here as
`docs/api/sync.md`; liquid #160 and #165 supply the library client, server, and codecs. Lake uses
that public contract and tests a client round trip without duplicating the protocol. Symbols
from the compiler (liquid's build graph knows every translation unit) arrive when Phase 4's
incremental engine lands. Search-index storage is specified by the search work; repository content
and its liquid-owned metadata remain on the customer store volume.

## Secrets, encryption and PKI (decision §12)

Clean capabilities, namespaced by customer, mounted per organization: static secrets (versioned
key-value with history, soft delete and metadata; #24), encryption as a service (encrypt,
decrypt, sign, verify, derive; the key never leaves lake; #25), certificate profiles (SPIFFE
X.509 and JWT SVIDs, an ACME endpoint, SSH and client and device certificates; #26), caller
authentication by attestation (Kubernetes service-account tokens, OIDC federation, cloud
instance identity; #27), and break-glass with a Shamir-recovered root key and a rehearsed quorum
(#28). Keys are wrapped by a per-customer KEK held in the control plane's key store (Postgres
today, an HSM-backed store when the operator provides one), the same envelope model liquid uses
for its store. Every operation is an audit event.

**Implemented status.** Issues #24 and #25 provide migration `0009_secrets`, organization mounts,
versioned static-secret lifecycle, named key rotation, encrypt/decrypt/rewrap, Ed25519 sign/verify,
HMAC-SHA-256, stored child-key derivation and customer-KEK rotation. The API, authorization,
signed audit fields and pages are documented in [`../api/secrets.md`](../api/secrets.md) and
[`../api/encryption.md`](../api/encryption.md). Certificate profiles, caller federation and
break-glass remain in #26–#28.

## The forge, the drive, models, metering, migration

Change review attaches to liquid's change identity, comments anchor to cursors that survive
edits, diffs are structural; CI is the build engine, not a product (#30). The drive is the same
repositories with per-folder ACLs, share links and a web surface through the format registry
(#8, #43, #44, #45). Models and datasets are repositories with training provenance (#7).
Metering counts unique stored bytes per customer store and never degrades to pointer files at a
quota (#33). Full export and the GitHub importer state their fidelity (#31, #32). The
transparency log (#34 to #38, #39) follows liquid's `docs/features/25` tiled, witnessed design
with the log as Zetta objects.

## Waves

The codebase is small and every leaf touches the registry and the schema, so the work runs in
waves that do not collide:

1. **Identity core** (#14 with #17), **authorisation** (#16 with #19), **audit** (#18),
   **push and pull with lifecycle** (#12 with #13), **search** (#6).
2. **SSO and federation** (#15, #21), **offline authorisation and recovery** (#22, #23),
   **secrets, encryption, certificates, callers, break-glass** (#24 to #28), **web UI** (#9),
   **query-time filtering** (#40).
3. **Transparency log** (#34 to #39), **forge, export, import** (#30 to #32), **drive and models**
   (#7, #8, #43 to #45), **search over history and inside artifacts** (#41, #42), **metering**
   (#33), **auth as a service** (#51).

Each wave rebases onto the previous; the schema grows by numbered migrations (the operator's
incremental-migration follow-up is a prerequisite for wave 2 and is raised there).

## How to verify

Each issue adds its proof beside the existing ones (`docs/api/README.md`, `docs/serving/`,
`docs/control-plane/`): unit tests on the pure renderers and the registry, a Postgres-backed
integration test per capability (the test container the CI job already provisions), and, for
push and pull, a liquid client round trip against a lake started in the test. The phase exit is
the plan's: a customer installs k0s and orama-operator on their own hardware, applies one `Lake`,
and pushes a liquid repository to it.

## How to change

This page is the implementation contract. An issue that finds it wrong changes it in the same
commit as the code and says why in the commit body.
