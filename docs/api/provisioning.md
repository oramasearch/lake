# Personal customer provisioning

Decision 40 in [liquid-bridge #26](https://github.com/oramasearch/liquid-bridge/issues/26)
requires a person signing up to own a customer. `sign_up_password` in the `lake` pool now
creates that tenant and its access grant before returning a browser session. The existing
registry entry serves REST, OpenAPI, MCP, and the hosted form with the same behavior.
Application pools remain isolated application identities; their sign-up does not allocate a
Lake customer or confer Lake authority.

## Naming, quotas, and metering

- Names use the existing generated `adjective-noun-number` handles. They reveal neither email
  nor a signing-key fingerprint. Handles are permanent; there is no customer rename capability.
  Repository names can still be changed through the existing repository lifecycle capability.
- A personal customer inherits `LAKE_STORAGE_QUOTA_BYTES`. There is no stored override on
  sign-up. The existing default is `0` (unlimited); an operator's configured limit therefore
  applies immediately. No repository-count or compute allotment is introduced. The existing
  authorized `set_storage_quota` capability can change the customer's quota.
- The tenant is an ordinary `customers` row, present in customer inventory and counted even
  with no repositories. No repository data is written during sign-up. Metering charges unique
  stored bytes, transfer, and compute, with no tenant-creation or minimum fee. The first usage
  read persists an hourly zero gauge and costs exactly zero even with nonzero unit prices.
  Control-plane user, customer, grant, and audit rows are outside the billable customer pack
  namespace. See [metering.md](metering.md).

## Atomicity and retry behavior

PostgreSQL uses one `pqxx::work` transaction for the user, verified public identity document,
password credential, customer, customer policy object, ownership grant, device, and session.
`with_tx` commits only a successful result. An exception or an unsuccessful session issuance
rolls back every row. No follow-up request from the browser is needed to finish provisioning.
No schema migration or external store write is required.

The customer has `user_id` equal to the new user. Its policy object inherits normally, and a
direct `admin` grant to the verified TAAP identity supplies ownership, including repository
creation and inherited access. The grant's `sign_up_password/<document-hash>` evidence records
which verified document established the bootstrap; it is a provenance reference, not a new
attestation format. `Registry::call` records the sign-up through the existing signed audit path.

Unique user identity and case-insensitive credential subject constraints serialize simultaneous
attempts across server processes. `customers.user_id` allows only one personal customer.
Generated names are prechecked; `ON CONFLICT (name) DO NOTHING` handles the race and retries
up to 32 candidates inside the same transaction. Exhaustion aborts all sign-up rows.

Retries are idempotent in persisted account and tenant state: a repeated successful sign-up
returns `409 conflict`, creates no second user, customer, grant, device, or session, and does not
reset a password or bypass MFA. After a lost success response, use **Sign in** with the original
credentials. After a rolled-back failure, retry sign-up. Existing accounts are not backfilled.

The in-memory test store mirrors customer ownership and stages password/session derivation
before publishing rows under its mutex. It retains its pre-existing synthetic identity model;
production document verification and rollback are tested against PostgreSQL.

## Identity document and first repository

The supported client command is `liquid identity show --document`: unpadded base64url canonical
CBOR, with the private key retained by Liquid. The sign-up page links the exact
[installation instructions](https://github.com/oramasearch/liquid/blob/main/docs/build/local.md)
and [identity reference](https://github.com/oramasearch/liquid/blob/main/docs/reference/cli/identity.md).
Malformed encoding, truncated CBOR, and invalid signatures remain refusals before user creation.
Encoding and CBOR errors say what failed and tell the visitor to export the complete document again.

A new account's first workspace, made by `start_workspace`, is named after the account's handle
(the person's or the organization's), not `workspace` (lake#638), so the top bar's picker reads a
name. An existing first workspace called `workspace` is never renamed by lake; its owner can rename
it (`update_workspace`, **Rename** on Workspaces). `Workspaces.StartWorkspaceNamesTheFirstWorkspaceAfterTheHandle`
checks both.

After sign-up, `/repositories` offers a private repository form once the account has a workspace;
before that it links to `/workspaces`, because every repository belongs to one workspace and there
is no default (lake#315). The form chooses the workspace, which names its account.
The web app calls `create_repository` through the registry and opens the created repository. An
unauthenticated caller or a user naming another customer's ID cannot create a repository there.
The homepage and navigation redesign remain [#135](https://github.com/oramasearch/lake/issues/135).

## Verify and change

Run the full native PostgreSQL-backed static and shared suites described in
[../build/local.md](../build/local.md). `PostgresProvisioning.*` checks ownership, immutable valid
names, absence of a quota override, repeated/concurrent requests, cross-customer denial, malformed
input, and complete rollback after a deliberately failed session insert followed by a successful
retry. `HttpResponseIsolation.FreshSignUpOwnsFirstRepositoryWithZeroInitialMetering` runs with
memory and PostgreSQL stores over real HTTP and the production repository store. It checks the
redirect, the workspace prompt and then the first-repository form, zero hourly storage and cost with nonzero prices, configured quota
inheritance, and first-repository access without an operator credential.

Changing provisioning must preserve these checks and the application pool boundary. Any future
handle rename or quota policy needs to update this contract and the registry capability first.
The visual walkthrough and recorded proof are in [../ui/sign-up-walkthrough.md](../ui/sign-up-walkthrough.md).
