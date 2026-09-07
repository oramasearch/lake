# The web UI

Drive browsing, preview, sharing, conflict history, end-to-end scenarios, and documented Linux client states are in
[drive.md](drive.md). The sharing decision and management screens are in
[sharing.md](sharing.md).
The forge page hierarchy and no-script behavior are in [`forge.md`](forge.md).
Organization and personal settings are in [`settings.md`](settings.md).

**What.** Server-rendered pages from pure renderers (`include/lake/api/web_ui.hpp`,
`src/api/web_ui.cpp`): one token-driven stylesheet at a content-hashed URL, content-hashed
self-hosted font URLs, an accessible shared shell, and a persisted light/dark theme control,
unit-tested as strings. The grouped navigation covers workspace, administration, applications,
identity, and developer pages; contextual links lead to exports, SSO, repository content, and
drive views. The unhashed stylesheet and font routes remain compatibility aliases with
`Cache-Control: no-cache`.

| Page | Route | Shows |
|---|---|---|
| Home | `GET /` | the node, version, links |
| User identity | `GET /sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in`, `/settings`, `/sessions`, `/passkeys`, `/tokens` | password, TOTP, OAuth, profile, passkeys, sessions/devices, notifications, and scoped tokens; see [identity.md](identity.md) |
| Applications | `GET /applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | registration, theme, pool users, organizations, webhooks, and delivery history; see [identity.md](identity.md) |
| Application identity | `GET /apps/<id>/sign-up`, `/sign-in`, `/mfa`, `/passkey-sign-in` | themed pool-specific identity on lake's host or a configured custom host |
| Operator sign in | `GET /login`, `POST /session` (form field `token`), `POST /logout` | the operator-token form; a match sets the session cookie and redirects to Repositories |
| Customers | `GET /customers` | every tenant: handle (links to its repositories), type, identity id, created |
| Organization settings | `GET /organization?id=<uuid>&tab=<tab>` | overview, teams, members, invitations, access, SSO, applications, metering, and audit; see [settings.md](settings.md) |
| Team settings | `GET /team?id=<uuid>` | team memberships and effective access; see [settings.md](settings.md) |
| Member settings | `GET /member?id=<uuid>` | role, attestation evidence, effective access, role change, and confirmed removal; see [settings.md](settings.md) |
| Organizations | `GET /organizations` | every authorized organization, with links to the organization settings surface |
| Teams | `GET /teams?organization_id=<uuid>` | the organization's authorized teams with member and effective-access links |
| Enterprise SSO | `GET /sso?organization_id=<uuid>[&provider_id=<uuid>]` | SAML/OIDC provider configuration, federation issuer claims, and one provider's SCIM provisioning log |
| Members | `GET /members?organization_id=<uuid>[&team_id=<uuid>]` | direct memberships, their role, principal and attestation evidence |
| Invitations | `GET /invitations?organization_id=<uuid>[&team_id=<uuid>]` | invitee, role, expiry and status |
| Grants | `GET /grants?object_id=<uuid>` | direct policy grants and their attestation evidence |
| Effective access | `GET /access?object_id=<uuid>` | direct and inherited principals, roles and allowed actions; object pages link to entity-specific views |
| Repositories | `GET /repositories[?customer_id=<uuid>]` | public repositories for anonymous readers, or authorized repositories for a signed-in caller |
| Repository | `GET /repository?id=<uuid>` | forge home with tree, recent changes, rendered README, refs and releases, clone/push commands, language/size summary, lifecycle state, and effective access |
| Environments | `GET /environments?repository_id=<uuid>` | current mirrored phase, change, short image digest, URL, domains, update time, and stale warning |
| Environment | `GET /environment?repository_id=<uuid>&name=<name>` | deployment history, URL and namespace, environment-variable and domain capability forms, and logs link |
| Environment logs | `GET /logs?repository_id=<uuid>&environment=<name>[&since=<cursor>]` | server-rendered last page and refresh link, enhanced by a small polling island using `environment_logs` |
| Model | `GET /models/<namespace>/<name>` or `/models/<uuid>` | card, revisions, file sizes and deduplication savings, storage totals, and the verified dataset-to-model provenance chain |
| Exports | `GET /exports?repository_id=<uuid>` | signed archive requests and prior export manifests; its download form calls the export capability |
| Repository tree | `GET /repository/tree?id=<uuid>&ref=<ref>` | the recursive libliquid tree with breadcrumbs, sizes, modes, and per-path last changes |
| File | `GET /file?repository_id=<uuid>&ref=<ref>&path=<path>`, `/file/raw`, `/file/download` | highlighted or plain source, line/range anchors, blame, backing blob, and raw/download responses |
| Changes | `GET /changes?repository_id=<uuid>` | the libliquid change log |
| Change | `GET /change?repository_id=<uuid>&id=<change>` | title and rendered message, compact identities, review state, file stats, unified/side-by-side text hunks, anchored comments, conflicts, checks, and attestations |
| Issues | `GET /issues?repository_id=<uuid>[&state=<state>&label=<label>&assignee=<identity>]` | filterable issue list with state, labels, assignees, and compact identities |
| Issue | `GET /issue?repository_id=<uuid>&id=<issue>` | sanitized Markdown body and discussion, labels, assignees, and linked changes |
| Releases | `GET /releases?repository_id=<uuid>` | sanitized release notes, change identity, artifacts, output counts, and verified attestations |
| Webhooks | `GET /webhooks?repository_id=<uuid>` | sanitized endpoints and events with delivery status, attempts, errors, and failed-delivery retry |
| Branch protection | `GET /protection?repository_id=<uuid>` | ref rules, required review and verification, update mode, pushers, and signed refusal audit |
| Conflicts | `GET /conflicts?repository_id=<uuid>`, `POST /conflicts/resolve` | conflict kind, exact region, explanation, suspicious-merge reasons, values, and resolution controls |
| Artifacts | `GET /artifacts?repository_id=<uuid>[&action=<digest>&output=<path>&content=<digest>&cursor=<cursor>&limit=<n>]` | filtered cursor pages of build results |
| Artifact | `GET /artifact?repository_id=<uuid>&id=<result>` | one result, outputs, timing, cache/divergence state, verified attestations, and trace summary |
| Search | `GET /search?query=<text>[&customer=<id>&repository=<id>&ref=<name>&change=<digest>&history=1]` | authorized hot branch-tip, immutable tag-snapshot, exact-change, or complete-history results with the selected query mode |
| Transparency monitor | `GET /transparency?identity=<taap-id>` | public checkpoint-verified bulk-tile scan and every matching TAAP attestation |
| Secret mounts | `GET /secret-mounts[?customer_id=<uuid>&organization_id=<uuid>]` | mounts the caller may read, with links to static secrets and named keys |
| Static secrets | `GET /secrets?mount_id=<uuid>[&name=<name>]` | custom metadata and version lifecycle; never secret values |
| Encryption keys | `GET /encryption-keys?mount_id=<uuid>` | key names, current versions and derivation parent; never key material |
| Drive folder | `GET /drive?repository_id=<uuid>&ref=<ref>&path=<path>[&view=list|grid&sort=name|size|modified&direction=asc|desc]` | breadcrumbs, folder and file cards or rows, size, modified time, and capability-backed create, upload, rename, move, and delete controls |
| Drive preview | `GET /drive/preview`, `/drive/download` | server-rendered or browser-native preview for registered formats, with exact-byte download, size, and digest fallback |
| Drive history | `GET /drive/history`, `/drive/history/diff`, `/drive/conflict`; `POST /drive/conflict/resolve` | retained path versions, restore-as-new-change controls, explained two-version conflicts, and write-authorized resolution |
| Drive sharing | GET /drive/share, /drive/share/link, /share/<token> | effective access before sharing, People/Organization/Link choices, link state and revocation consequence, individual link detail, and the public shared-folder view; see [sharing.md](sharing.md) |
| Shared with me | GET /shared-with-me | live folders granted directly to the signed-in identity, their grantor, allowed actions, and current availability; see [sharing.md](sharing.md) |

**Session.** `POST /session` compares the form token with the operator token in constant time; on
a match it sets `lake_session=<token>; HttpOnly; SameSite=Strict; Path=/` (`Secure` when the
request arrived over HTTPS, read from `X-Forwarded-Proto`) and redirects. Every page that reads
the store requires it and redirects to `/login` otherwise; `POST /logout` clears it. The cookie
value is the same secret the API takes as a bearer, resolved by the same code (`context_for`), so
the UI never has a second auth path. User sign-in forms call registry capabilities and set the
same cookie. A wrong operator token answers `401` with the form and a message.
Public repository routes are the deliberate exception: they render without a session only after
the repository capability verifies `visibility=public`; other object pages redirect to sign-in.
The transparency monitor is also public because independent checkpoint and tile verification must
not depend on a Lake account.

**Data.** Data pages call the same registry capabilities as REST and MCP, with the session's
principal and one per-request authorization cache. Lists are capped by their capability limits
and returned newest first. Nothing on a page can do what the API cannot.

**Lifecycle.** The repository page renders a deletion tombstone and its retention deadline when
present. The signed audit page exposes the registry outcomes for lifecycle capability calls.
Lifecycle mutations remain capability-only; the page has no mutation form.

Repository-page details and the liquid #166 deferrals are recorded in
[`repositories.md`](repositories.md).

The reproducible seeded screenshot workflow and complete before/after page inventory are recorded
in [`design-system.md`](design-system.md). The fixture uses the production HTTP and renderer paths;
it does not add a fixture route to the server.

Secret mutations and cryptographic operations are also capability-only. The secrets page renders
metadata and lifecycle timestamps, while the encryption page renders key metadata. Neither page
puts protected input or output into HTML.

**Verify.** `./build/lake --listen 127.0.0.1:4902 --memory-store --admin-token t`, then with a
cookie jar: `curl -c j -d token=t /session` (302), `curl -b j /organizations` (200, the table),
`curl -b j -X POST /logout` (302), `curl -b j /repositories` (302 to `/login`).

**Next.** A customer detail page and mutation forms for the remaining control-plane actions.
