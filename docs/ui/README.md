# The web UI

**What.** Server-rendered pages from pure renderers (`include/lake/api/web_ui.hpp`,
`src/api/web_ui.cpp`): one stylesheet at `/style.css`, no script, unit-tested as strings. The
navigation is on every page: Home, Customers, Repositories, API (`/v1/openapi.json`), and
Sign in / Sign out.

| Page | Route | Shows |
|---|---|---|
| Home | `GET /` | the node, version, links |
| Sign in | `GET /login`, `POST /session` (form field `token`), `POST /logout` | the admin-token form; a match sets the session cookie and redirects to Repositories |
| Customers | `GET /customers` | every tenant: handle (links to its repositories), type, identity id, created |
| Repositories | `GET /repositories[?customer_id=<uuid>]` | every repository newest first as `<owner>/<name>`, owner, visibility, description, created |
| Repository | `GET /repository?id=<uuid>` | one repository: owner, visibility, timestamps, lifecycle state, audit history, and the API call that returns it |

**Session.** `POST /session` compares the form token with the admin token in constant time; on
a match it sets `lake_session=<token>; HttpOnly; SameSite=Strict; Path=/` (`Secure` when the
request arrived over HTTPS, read from `X-Forwarded-Proto`) and redirects. Every page that reads
the store requires it and redirects to `/login` otherwise; `POST /logout` clears it. The cookie
value is the same secret the API takes as a bearer, resolved by the same code (`context_for`), so
the UI never has a second auth path. A wrong token answers `401` with the form and a message.

**Data.** Pages read the `Store` directly (the same interface the capabilities use), up to 500
rows, newest first. Nothing on a page can do what the API cannot.

**Lifecycle.** The repository page renders a deletion tombstone and its retention deadline when
present, followed by the append-only audit entries returned by `list_repository_audit_events`.
Lifecycle mutations remain capability-only; the page has no mutation form.

**Verify.** `./build/lake --listen 127.0.0.1:4902 --memory-store --admin-token t`, then with a
cookie jar: `curl -c j -d token=t /session` (302), `curl -b j /repositories` (200, the table),
`curl -b j -X POST /logout` (302), `curl -b j /repositories` (302 to `/login`).

**Next.** Creating repositories from the UI (today: `POST /v1/create_repository`), a customer
page, and TAAP sign-in when identities land (lake issue #4).
