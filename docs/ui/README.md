# The web UI

**What.** Server-rendered pages from pure renderers (`include/lake/api/web_ui.hpp`,
`src/api/web_ui.cpp`): one stylesheet at `/style.css`, with inline script only on the two passkey
pages that call `navigator.credentials`, unit-tested as strings. The navigation is on every page:
Home, Customers, Organizations, Repositories, Search, Secrets, Audit, SSO, API
(`/v1/openapi.json`), and Sign in / Sign out.

| Page | Route | Shows |
|---|---|---|
| Home | `GET /` | the node, version, links |
| User identity | `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in`, `/sessions`, `/passkeys`, `/tokens` | password, TOTP, OAuth, passkeys, sessions/devices, and scoped tokens; see [identity.md](identity.md) |
| Operator sign in | `GET /login`, `POST /session` (form field `token`), `POST /logout` | the operator-token form; a match sets the session cookie and redirects to Repositories |
| Customers | `GET /customers` | every tenant: handle (links to its repositories), type, identity id, created |
| Organizations | `GET /organizations` | every authorized organization, with links to teams, members, and effective access |
| Teams | `GET /teams?organization_id=<uuid>` | the organization's authorized teams with member and effective-access links |
| Enterprise SSO | `GET /sso?organization_id=<uuid>[&provider_id=<uuid>]` | SAML/OIDC provider configuration, federation issuer claims, and one provider's SCIM provisioning log |
| Members | `GET /members?organization_id=<uuid>[&team_id=<uuid>]` | direct memberships, their role, principal and attestation evidence |
| Invitations | `GET /invitations?organization_id=<uuid>[&team_id=<uuid>]` | invitee, role, expiry and status |
| Grants | `GET /grants?object_id=<uuid>` | direct policy grants and their attestation evidence |
| Effective access | `GET /access?object_id=<uuid>` | direct and inherited principals, roles and allowed actions; object pages link to entity-specific views |
| Repositories | `GET /repositories[?customer_id=<uuid>]` | public repositories for anonymous readers, or authorized repositories for a signed-in caller |
| Repository | `GET /repository?id=<uuid>` | metadata, refs, content links, lifecycle state, and effective access |
| Repository tree | `GET /repository/tree?id=<uuid>&ref=<ref>` | the recursive libliquid tree at a published ref |
| File | `GET /file?repository_id=<uuid>&ref=<ref>&path=<path>` | one file, backing blob and escaped contents |
| Changes | `GET /changes?repository_id=<uuid>` | the libliquid change log |
| Change | `GET /change?repository_id=<uuid>&id=<change>` | change identity, metadata, per-path status, modes, blobs, text hunks, moves, and conflict values |
| Conflicts | `GET /conflicts?repository_id=<uuid>` | unresolved conflict values |
| Artifact | `GET /artifact?repository_id=<uuid>&id=<result>` | result identity; attestations await liquid #166 |
| Search | `GET /search?query=<text>` | ranked text, quoted substring, or regex results that the signed-in caller may read |
| Secret mounts | `GET /secret-mounts[?customer_id=<uuid>&organization_id=<uuid>]` | mounts the caller may read, with links to static secrets and named keys |
| Static secrets | `GET /secrets?mount_id=<uuid>[&name=<name>]` | custom metadata and version lifecycle; never secret values |
| Encryption keys | `GET /encryption-keys?mount_id=<uuid>` | key names, current versions and derivation parent; never key material |

**Session.** `POST /session` compares the form token with the operator token in constant time; on
a match it sets `lake_session=<token>; HttpOnly; SameSite=Strict; Path=/` (`Secure` when the
request arrived over HTTPS, read from `X-Forwarded-Proto`) and redirects. Every page that reads
the store requires it and redirects to `/login` otherwise; `POST /logout` clears it. The cookie
value is the same secret the API takes as a bearer, resolved by the same code (`context_for`), so
the UI never has a second auth path. User sign-in forms call registry capabilities and set the
same cookie. A wrong operator token answers `401` with the form and a message.
Public repository routes are the deliberate exception: they render without a session only after
the repository capability verifies `visibility=public`; other object pages redirect to sign-in.

**Data.** Data pages call the same registry capabilities as REST and MCP, with the session's
principal and one per-request authorization cache. Lists are capped by their capability limits
and returned newest first. Nothing on a page can do what the API cannot.

**Lifecycle.** The repository page renders a deletion tombstone and its retention deadline when
present. The signed audit page exposes the registry outcomes for lifecycle capability calls.
Lifecycle mutations remain capability-only; the page has no mutation form.

Repository-page details and the liquid #166 deferrals are recorded in
[`repositories.md`](repositories.md).

Secret mutations and cryptographic operations are also capability-only. The secrets page renders
metadata and lifecycle timestamps, while the encryption page renders key metadata. Neither page
puts protected input or output into HTML.

**Verify.** `./build/lake --listen 127.0.0.1:4902 --memory-store --admin-token t`, then with a
cookie jar: `curl -c j -d token=t /session` (302), `curl -b j /organizations` (200, the table),
`curl -b j -X POST /logout` (302), `curl -b j /repositories` (302 to `/login`).

**Next.** Mutation forms for the remaining control-plane actions and a customer detail page.
