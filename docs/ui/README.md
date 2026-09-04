# The web UI

**What.** Server-rendered pages from pure renderers (`include/lake/api/web_ui.hpp`,
`src/api/web_ui.cpp`): one stylesheet at `/style.css`, no script, unit-tested as strings. The
navigation is on every page: Home, Customers, Organizations, Repositories, Search, Audit, API
(`/v1/openapi.json`), and Sign in / Sign out.

| Page | Route | Shows |
|---|---|---|
| Home | `GET /` | the node, version, links |
| Sign in | `GET /login`, `POST /session` (form field `token`), `POST /logout` | the admin-token form; a match sets the session cookie and redirects to Repositories |
| Customers | `GET /customers` | every tenant: handle (links to its repositories), type, identity id, created |
| Organizations | `GET /organizations` | every organization, with links to its teams and members |
| Teams | `GET /teams?organization_id=<uuid>` | the organization's teams and member links |
| Members | `GET /members?organization_id=<uuid>[&team_id=<uuid>]` | direct memberships, their role, principal and attestation evidence |
| Invitations | `GET /invitations?organization_id=<uuid>[&team_id=<uuid>]` | invitee, role, expiry and status |
| Grants | `GET /grants?object_id=<uuid>` | direct policy grants and their attestation evidence |
| Effective access | `GET /access?object_id=<uuid>` | direct and inherited principals, roles and allowed actions |
| Repositories | `GET /repositories[?customer_id=<uuid>]` | every repository newest first as `<owner>/<name>`, owner, visibility, description, created |
| Repository | `GET /repository?id=<uuid>` | one repository: owner, visibility, timestamps, lifecycle state, and the API call that returns it |
| Search | `GET /search?query=<text>` | ranked text, quoted substring, or regex results that the signed-in caller may read |
| Audit | `GET /audit` | signed and hash-linked capability and session outcomes |

**Session.** `POST /session` compares the form token with the admin token in constant time; on
a match it sets `lake_session=<token>; HttpOnly; SameSite=Strict; Path=/` (`Secure` when the
request arrived over HTTPS, read from `X-Forwarded-Proto`) and redirects. Every page that reads
the store requires it and redirects to `/login` otherwise; `POST /logout` clears it. The cookie
value is the same secret the API takes as a bearer, resolved by the same code (`context_for`), so
the UI never has a second auth path. A wrong token answers `401` with the form and a message.

**Data.** Data pages call the same registry capabilities as REST and MCP, with the session's
principal and one per-request authorization cache. Lists are capped by their capability limits
and returned newest first. Nothing on a page can do what the API cannot.

**Lifecycle.** The repository page renders a deletion tombstone and its retention deadline when
present. The signed audit page exposes the registry outcomes for lifecycle capability calls.
Lifecycle mutations remain capability-only; the page has no mutation form.

**Verify.** `./build/lake --listen 127.0.0.1:4902 --memory-store --admin-token t`, then with a
cookie jar: `curl -c j -d token=t /session` (302), `curl -b j /organizations` (200, the table),
`curl -b j -X POST /logout` (302), `curl -b j /repositories` (302 to `/login`).

**Next.** Mutation forms (today the create operations are `/v1` actions), a customer page, and
TAAP sign-in when credential resolution lands (lake issue #4).
