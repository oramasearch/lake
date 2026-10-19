# The web app's capabilities

**Status: implemented in the source tree (lake#561); not deployed.** lake's web UI is being rebuilt
as a React app (`frontend/`, [`../ui/README.md`](../ui/README.md)) that calls only the `/v1`
capability API. The server-rendered pages computed several things for themselves that no capability
returned. These capabilities return them, so the web app can do everything through `/v1`, and so
can an API or MCP caller. Each is registered in the one registry: REST, OpenAPI, MCP, authorization
in `Registry::call` and audit come with it. Where a page computed the same thing, the page now
calls the capability or the shared function, so the two cannot disagree.

| Capability | Kind | Who | Input | Value |
|---|---|---|---|---|
| `viewer` | read | **anyone** | `workspace_id?`, `customer_id?` | who is calling and how this lake signs people in (below) |
| `my_permissions` | read (also `POST`) | signed in | `object_type` (a policy object type, `workspace` for its account's permissions, or `drive folder`; another type is refused listing them, lake#594), `object_id`, `actions`, `ref?`, `path?` | `{allowed: {<action>: bool}}` |
| `list_packages` | read | signed in | `customer_id?` | `{available, origin, npm, python, oci}` |
| `list_docs` | read | **anyone** | `q?` (every word, in any order, lake#594) | `{pages: [{slug, title, section, headings, match?, anchor?, excerpt?}], sections}` |
| `get_doc` | read | **anyone** | `slug?` (`page` is its deprecated name, lake#591) | `{slug, title, section, markdown, previous?, next?}` (each `{slug, title, section}`) |
| `mcp_setup` | read | signed in | — | `{origin, mcp_url, tool_groups, flows, scopes}` |
| `resolve_model` | read | anyone, for a public model | `namespace?`, `name` | `{repository_id, revision}` |
| `latest_agent_task` | read | read on the account | `customer_id`, `workspace_id`, `subject` | `{task: <get_agent_task value> \| null}` |
| `compile_pipeline` | action | write on the account | `customer_id`, `workspace_id`, `project`, `intent`, `dataset?`, `label_column?`, `entity_column?`, `methodology?`, `name?` | `{graph, template?}` |
| `render_pipeline` | read | read on the account | `customer_id`, `workspace_id`, `project`, `graph`, `step?` | `{svg}` |

The Ask Orama and Agent Factory conversation (`send_ask_message`, `settle_ask_turn`,
`confirm_ask_proposal`, `approve_agent_proposal`) is in [ask-orama.md](ask-orama.md#capabilities-lake561)
and [agent-factory.md](agent-factory.md). `repository_file` gained `units` for the File page
([repositories.md](repositories.md#repository-data-and-change-pages)), `get_organization` names the
organization's own account as `customer_id` (null when it has none), and `list_sessions` rows declare
the `reason` and `last_used_at` they already carried.

## viewer

`GET /v1/viewer?workspace_id=<optional>&customer_id=<optional>`. It needs no credential and never
answers `401`: a visitor, or a browser holding a dead cookie, gets `signed_in: false` and no accounts.

| Field | Always | What |
|---|---|---|
| `signed_in`, `administrator` | yes | a credential was accepted; it is the operator credential |
| `user_id`, `taap_id`, `session_id`, `credential_kind`, `display_name`, `email` | a person | the person and the credential this request carries (`session_id` is what `revoke_session` takes) |
| `mfa_enrolled` | yes | the person has an active TOTP enrollment (`Store::mfa_enrolled`); false for a visitor and the operator |
| `spaces` | yes | what `list_customers` gives the caller: `{id, name, label, personal, kind, workspaces}`; `label` is *Your workspaces* for the person's own individual account, else the name; `workspaces` are its live ones (`desired` not `deleted`) as `{id, name, customer_id, status}`, `status` the operator's phase in lower case (`pending` until it reports) |
| `customer_id` | when there is one | the account asked for when the caller can see it, else the account of the workspace asked for, else the first |
| `workspace_id` | when there is one | the workspace asked for when it is live in one of the caller's accounts, else that account's only workspace, else absent |
| `clerk` | yes | `{enabled, publishable_key, frontend_api}` from `LAKE_CLERK_PUBLISHABLE_KEY` ([clerk.md](clerk.md)) |
| `password_sign_up` | yes | lake's own sign-up is open: false under Clerk, where accounts are added by invitation |
| `external_origin`, `public_host` | yes | lake's browser origin (`LAKE_EXTERNAL_ORIGIN`, else `https://<LAKE_PUBLIC_HOST>`, else the origin of the request) and `LAKE_PUBLIC_HOST`; clone and push URLs use the origin |
| `config` | yes | `{frontier_model}` for Create an Agent (`LAKE_AGENT_MODEL`); empty for a visitor. The base model's path (`LAKE_BASE_AGENT_MODEL`) is no longer answered (lake#594): `workspace_act create_agent` fills it in for a `native` agent sent without `model` |
| `version`, `revision` | yes | as `server_info` reports them |

An operator credential has `administrator: true` and no user fields. A `workspace_id` that names no
workspace at all is `invalid_input` at `/workspace_id` (the registry checks every `workspace_id` it
is given); a deleted one, or one outside the caller's accounts, is replaced as above. Under
`/w/<workspace_id>/v1/viewer` the path's workspace is the one asked for.

## my_permissions

Whether the caller may take each of `actions` on one object, for the buttons a page shows only when
they would work: Experiments' start and promote, Streams' writes and drops, Drive's Share. It never
replaces the capability's own check.

- `object_type` and `object_id` name a policy object: `customer` and an account id, `repository` and a
  repository id, `organization`, `team`, and the other object types `find_policy_object` knows.
- `object_type: "drive folder"` names a repository by `object_id` and a folder by `path` at `ref`
  (default `main`), resolved exactly as the Drive capabilities authorize it: the nearest folder with
  its own policy object, else the repository.
- `actions` are `read`, `write`, `admin`, `deploy`, `review`, `share` and `promote`. `promote` is
  `admin` on the object: promoting a trial is an account administrator's decision.
- An unknown object is `not_found` at `/object_id`. The operator may do everything.

For example, `POST /v1/my_permissions` (a `GET` takes `actions` as a JSON array in the query):

```json
{"object_type": "customer", "object_id": "8f1c…", "actions": ["write", "promote"]}
```

## list_packages

Ship → Packages: npm packages, Python projects and one-component OCI images in this lake's instance
registries (`ArtifactRegistry::catalog()`, each `{name, versions}`), all public reads; with
`customer_id`, also the account's own images (from `list_image_repositories`, called as the caller,
without its `cache/` repositories). `available` is false when this lake has no registry storage;
`origin` is the origin install commands use (`LAKE_EXTERNAL_ORIGIN`, else `https://<LAKE_PUBLIC_HOST>`,
else empty). The Packages page reads this capability.

## list_docs and get_doc

The Docs guides (`docs/guide/*.md`, embedded at build time) in reading order. `get_doc` without
`page` is the first, Getting Started; an unknown page is `not_found` at `/page`. The browser renders
the Markdown.

**Sections (lake#561).** `cmake/embed_guide.cmake`'s `GUIDE_SECTIONS` puts every page under a
section (Get Started, Data, Design, Train, Ship, Organization, Developer, Reference), and a
section's pages are adjacent. `list_docs` answers `sections`, the names in order, and each page's
`section`; `slug` and `title` are unchanged. `get_doc` answers the page's `section` and the
`previous` and `next` pages, each `{slug, title, section}`, absent at either end.

**Headings.** Each `list_docs` page carries `headings`: its level 2 and 3 headings as
`{level, text, anchor}`. `anchor` is the id both renderers give the heading (`markdown_anchor`,
with a repeated heading numbered `-2`, `-3`, counted over every `#`–`####` heading outside code
blocks), so `/docs/<slug>#<anchor>` opens it. `text` drops inline-code and emphasis markers.

**Search.** With `q` (at most 200 characters, else `invalid_input` at `/q`), `list_docs` keeps the
pages whose title, a heading or the text contains it, ignoring ASCII case. Title matches come
first, then heading matches, then text matches, each in reading order. A match says what matched
(`match`: `title`, `heading` or `text`), the `anchor` of the matching heading or of the heading
above the first text match, and an `excerpt` of the text around the first match (whitespace
collapsed, never splitting a character). The Docs page's search box and Data → Search's Docs place
both read it.

Tested by `Api.*` in `tests/test_api.cpp` (the docs block), and `Docs.*` in
`tests/test_markdown.cpp`: every guide file is embedded once under one section, links between
guides and their anchors resolve, and no guide says `space`.

## mcp_setup

What Developer → MCP shows, for the web app's copy-ready setup: `origin` and `mcp_url`
(`<origin>/mcp`); `tool_groups`, every tool in the product groups `agent_docs::tool_groups` makes,
each group `{title, summary, tools: [{name, title, writes}]}`; `flows`, the worked flows
`{title, summary, steps: [{tool, note}]}`; and `scopes`, the tool names a read-only key
(`read_only`) and a key that never expires but names every tool (`all`) carry. The key itself is
made with `create_token`.

## resolve_model

The model page's `space/name` (`/models/:namespace/:id`): the repository `find_hub_repository`
finds for `namespace/name` (or `name` alone when it is unique), and the revision the page shows
(`main`). A private model the caller may not read is `not_found`, as if it did not exist; a public
one needs no credential. Read the model with `get_model {id: repository_id, revision}`. The Hub
routes and the model page use the same `find_hub_repository`.

## latest_agent_task

The project page's background tasks. `subject` is `build:<project>` (the Project Agent's pipeline
design for a project made from an Opportunity) or `solution:<project>` (its solution foundation
proposal); anything else is `invalid_input` at `/subject`. The newest task of that kind in the
workspace is read as `get_agent_task` reads it, so a finished design is applied to the project once
(`settle_build`), and `build_error` says why one could not be. `task` is null when there is none,
or when it is another person's turn. The project page reads this capability. As query parameters
of `GET /w/<workspace_id>/v1/latest_agent_task`, or as MCP arguments:

```json
{"customer_id": "8f1c…", "workspace_id": "4b2e…", "subject": "build:churn"}
```

## compile_pipeline

The project page's Build a Pipeline, as one call, with exactly what its form posts: capture
`intent` as the project's intent (`capture_intent`), compile it (`compile_intent`, with
`methodology` that methodology's own pipeline), and with `dataset` (`"<database>.<table>"`) point
the new pipeline's first connect step at it (`get_graph`, then `edit_pipeline`). The value names
the new pipeline as `graph` (empty when the engine named none) and, from an engine with zetta#1238,
`template`: the template its steps started from (`"the M-1 churn template"`). Lake names the
pipeline, not the engine (lake#816): `name` when the caller gives one (trimmed; a blank one is no
name), else a short title from the
intent (`pipeline_title`: *Predict which accounts will churn in the next 90 days* is *Accounts
churn*), numbered ` (2)`, ` (3)` past a pipeline of the project that has it, and passes that name to
`compile_intent`. Build a Pipeline shows the same title (`pipelineTitle` in the web app, tested on
the same cases) and sends what the person leaves or types there as `name`. The intent itself stays
the project's description. The step names (`load_customers`, `notify_retention` …) are the
template's until edited, and Build a Pipeline says so when it finishes. Each step is its own audited
capability call. The web app's project page calls it. For example,
`POST /w/<workspace_id>/v1/compile_pipeline`:

```json
{"customer_id": "8f1c…", "workspace_id": "4b2e…", "project": "churn", "intent": "Predict which accounts will churn in the next 90 days", "dataset": "datasets.accounts"}
```

## render_pipeline

The pipeline graph as inline SVG, drawn by `pipeline::render_svg` exactly as the pipeline page draws
it: one column per stage, data-flow arrows solid and control arrows dashed, the steps the latest run
of this pipeline ran coloured by its outcome, `step` outlined. Every text in the markup is escaped
by lake, so the page inserts it as it is. Every step is an `<a>` whose `href` is
`/w/<workspace_id>/design/pipeline?customer_id=<space>&project=<project>&graph=<graph>&step=<step id>`,
the address the server-rendered page used; the web app catches clicks on links carrying `step=` and
selects that step itself. As query parameters of `GET /w/<workspace_id>/v1/render_pipeline`, or
as MCP arguments:

```json
{"customer_id": "8f1c…", "workspace_id": "4b2e…", "project": "churn", "graph": "churn-pipeline", "step": "train"}
```

## The browser session

The web app is served from lake's own origin and authenticates with the `lake_session` cookie, as
the pages do. A cookie-authenticated unsafe request must come from lake's origin, and a `/v1` or
`/mcp` `POST` must be `application/json` ([cross-site requests](../serving/http.md#cross-site-requests)).
Sign-in returns: the Ship app return travels in `/sign-in?return_to=` as well as in the
`lake_sign_in_return` cookie ([web-apps.md](web-apps.md#lake-protected-app-authentication)), Clerk's
exchange honours a posted `next` ([clerk.md](clerk.md#how-it-works)), and a wrong operator token
redirects to `/login?error=…` ([front-door.md](../ui/front-door.md)).

## Verify

- `WebApp.PermissionsDocsMcpAndPackagesAreCapabilities` and
  `WebApp.ViewerNamesClerkAndAnOrganizationNamesItsSpace` (registry, memory store): the operator's
  and a writer's permissions, unknown objects and actions, the Docs in order, `mcp_setup`'s groups
  and scopes, the packages catalog with and without registry storage, Clerk in `viewer`, and an
  organization's account.
- `HttpServer/HttpResponseIsolation.TheViewerSaysWhoIsSignedInAndWhereTheyAre` (memory and
  PostgreSQL): a visitor and a dead cookie, a person's identity, MFA before and after enrolling,
  accounts and the current workspace, a foreign workspace refused as current, the operator.
- `HttpServer/HttpResponseIsolation.OperatorSignInSaysWhyATokenWasRefused`: the operator token
  error, and the right token's `SameSite=Strict` session cookie. `return_to` and `next` are
  handled by the web app.
- `Repositories.PublicRepositoryContentCapabilitiesUseTheRepositoryStore`: `units` and
  `resolve_model`.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every one of these
  capabilities successfully over MCP and validates each value against its declared output schema.
- By hand: `./build/lake --listen 127.0.0.1:4901 --memory-store --admin-token t`, then
  `curl -s localhost:4901/v1/viewer` (a visitor) and
  `curl -s -H 'Authorization: Bearer t' localhost:4901/v1/mcp_setup`.

## Change

A field the web app reads is added to the capability's output schema in the same change, with its
success fixture in `tests/test_schema_conformance.cpp`; the schemas here are closed
(`additionalProperties: false`), so an undeclared field fails the conformance proof.
