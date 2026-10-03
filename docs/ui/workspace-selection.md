# Workspace Selection and Navigation

The top bar has one space selector and one workspace selector. Workspaces are non-deleted contract rows
of that space. The viewer's own personal space is labelled *Personal space*, not its generated
handle; other spaces keep their names. A workspace is labelled by its name alone (names are unique
among a space's live workspaces); its environment is listed on `/workspaces` (lake#452). A sole workspace is selected but remains visible; selection is sent explicitly.
Changing space refreshes its workspace choices. New workspace opens `/workspaces`, which lists
name, environment, phase, namespace and created time, and accepts name/environment for creation.
It writes desired state through `create_workspace`; the operator provisions the namespace and
Zetta. Delete opens a confirmation on the same page (`/workspaces?delete=<workspace_id>`) that
lists what goes with the workspace, with counts: its Zetta and every record in it, its repositories
(live and deleted but still retained), its apps, its unfinished builds and its images. It states
that the deletion cannot be undone and requires typing the workspace name; a wrong name returns to
the confirmation with the error ([deletion](../api/workspaces.md#deletion), lake#514). Pending and
stopped workspaces remain selectable.
IDs are `workspaces.id`, not service IDs. This management page is outside the `/w/` prefix so the
first workspace can be created before a selection exists.
Switching from Workspaces stays on that management page. Explicit space selection takes
precedence over a remembered workspace from another space on control-plane pages.

Design, Train, Ship, Ask and the workspace proxy use `/w/<workspace_id>/...`. `/w/<workspace_id>`
and `/w/<workspace_id>/` (GET or HEAD) redirect to the workspace's home,
`/w/<workspace_id>/design/projects`, keeping the query; the home request validates the ID as every
workspace page does (lake#452). Navigation ownership
is evaluated on the local page path, so `aria-current="page"` follows the selected page while
links retain the workspace prefix. Shared rendering carries the ID into URL attributes, inline
navigation scripts, and hidden form fields. It does not rewrite ordinary hidden field values.
Live app/build, Streams and Experiments requests send the same ID to REST explicitly.

URL rewriting uses one forward scan with tracked script state and append-only output. Hidden
workspace fields use a separate forward pass over forms. Both are linear in document size:
large run lists do not repeatedly search earlier HTML or shift the remaining document for each
link. Already-prefixed URLs and forms with an existing workspace selector or hidden field remain
unchanged, so applying the rewrite again is safe. Global links, external links and ordinary
hidden values retain their original targets.

When changing URL generation, run `ctest --test-dir build -R '^WebUi.Workspace'` and the
`PagePerformance` test with PostgreSQL configured. The rewrite tests cover mixed script and
attribute URLs, query strings and fragments, explicit other-workspace targets, incomplete forms,
and 2,000-row documents. The [page budget](performance.md) includes this rendering work.

At phone width (`max-width: 40rem`) the top bar wraps: the actions keep the first row, and the
selectors (`.workspace-picker`: two equal columns and *Switch*) and the trail take a full-width row
below, so a 390 px viewport shows the whole space and workspace names instead of `gr…` and `wo…`.
The taller bar is not sticky there, and table headers stick to the top. `WebUi.PhoneWidthGivesTheWorkspacePickerItsOwnRow`
checks the rendered order and these rules; run `tools/ui/test-token-gates.sh` after changing them.

The selection form submits to `/workspace/select`. Switching keeps the page and query filters,
removing old customer/workspace query values. Lake records `lake_workspace`, an HttpOnly,
SameSite=Lax navigation cookie. It is never an API default or an authorization credential.
Every remembered selection is checked against the caller's accessible spaces and workspace rows.
Within one GET request, middleware, the handler and navigation share the resolved credential
context and the authorized customer-list result. A subsequent request resolves both again;
session revocation, new spaces and workspace selection changes are not cached across requests.
POST handlers do not reuse a pre-mutation page context. Capability authorization and auditing
still run for each distinct capability call.
Old GET routes redirect to the remembered workspace or the space's sole workspace. With several
workspaces and no valid selection, Lake asks the person to choose. Old mutation URLs are refused.

The canonical path is authoritative. A contradictory query workspace/customer is refused;
workspace form posts require one `workspace_id` matching the path. REST/MCP callers do not get a
cookie-based fallback. Task-bound and app credentials cannot cross workspace boundaries.

Coverage includes pure navigation/form rendering, raw HTTP legacy redirects, remembered selection,
same-page switching with filters, explicit mismatched/missing form refusals, and new-workspace
provisioning. Existing page scenarios select their test workspace explicitly before making
requests; their workspace-aware HTTP helper sends the same path and hidden field a browser uses.

`WorkspaceNavigationHttp.SelectorRedirectsRememberedWorkspaceAndValidatesForms` exercises the
server without that helper's selection support. `WebUi.WorkspacePathOwnsNavigationAndEveryFormCarriesTheSelection`
checks active navigation and repeated rendering. The HTTP page scenarios cover Ask, Agent Factory,
pipelines, Streams, apps and experiments with explicitly selected workspaces.

The storage rule, migration, retention, cleanup and cross-repository follow-ups are in the
[workspace data-plane design](../architecture/workspace-data-plane.md).
