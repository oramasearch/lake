# Workspace Selection and Navigation

**What.** Design, Train, Ship and Ask Orama work in one workspace of one space, and the page URL
says which: `/w/<workspace_id>/design/projects`. The top bar's picker changes it. Everything else
(repositories, organizations, account pages, Services, Deployments, Workspaces) is outside `/w/`
and names its space or workspace in its own input.

**Why the URL.** The address is the whole context: it can be bookmarked, shared and opened in two
tabs at once with two workspaces, and the capability calls a page makes follow from it. Before
lake#561 the server remembered the choice in a `lake_workspace` cookie and rewrote every link and
form into `/w/<id>/`; both are gone.

## The URL

- **A workspace page** (`workspace: true` in its `routes.ts`) answers at `/w/<workspace_id><path>`.
  Its query parameters are unchanged (`/w/<id>/design/project?name=churn`).
- **Its unprefixed address** (`/design/projects`, from an old link or the docs) opens the same page
  in the current workspace: the remembered one when the `viewer` read accepts it, else the only
  workspace across the viewer's spaces. With neither it shows *Choose a Workspace* with a link to
  Workspaces. A visitor is sent to sign in (`WorkspaceRedirect` in `src/App.tsx`).
- **`/w/<workspace_id>` and `/w/<workspace_id>/`** open the workspace's home,
  `/w/<workspace_id>/design/projects`, keeping the query.
- **The rail** marks the current item by the path without its `/w/<id>` prefix (`localPath` in
  `routes.tsx`), and its Design, Train and Ship links carry the current prefix.

## Only `/w/` pages scope API calls

`src/lib/session.tsx` sets the prefix from the page URL, and `src/lib/api.ts` sends every
capability called from a `/w/<id>/` page to `/w/<id>/v1/<name>`. Lake fills `workspace_id`
from that prefix and answers `invalid_input` when the input names a different one
([../serving/http.md](../serving/http.md#workspace-api-prefix)). A page outside `/w/`
calls `/v1/<name>` unprefixed, whatever workspace was used last, and passes `customer_id` or
`workspace_id` itself where the capability needs one (the Workspaces page, Services, the
repository create form, Drive sharing, secret mounts).

## The remembered workspace

The browser keeps the last workspace it opened under `/w/` in `localStorage["lake-workspace"]`.
It is navigation memory only:

- it chooses where an unprefixed workspace link goes, and which workspace the rail's links open
  from a page outside `/w/`;
- it is sent as `workspace_id` to the `viewer` read, which accepts it only when it is live in one
  of the caller's spaces and otherwise answers that space's only workspace, or none
  ([../api/web-app.md](../api/web-app.md#viewer));
- it never scopes an API call and is never a credential. Clearing it changes nothing a capability
  does.

## The picker

`components/WorkspacePicker.tsx`, shown to a signed-in viewer with at least one space:

- **Space** lists the viewer's spaces. The viewer's own personal space is labelled *Personal
  space*, not its generated handle; other spaces keep their names. Choosing a space with exactly
  one live workspace opens it; otherwise it opens `/workspaces?customer_id=<space>` to choose or
  create one.
- **Workspace** lists the chosen space's live workspaces by name (names are unique among a space's
  live workspaces; the environment is on Workspaces, lake#452). Pending and stopped workspaces are
  listed. When the URL names no workspace of that space it reads *Choose a workspace*.
- Choosing a workspace keeps the page: a workspace page reopens at the same path and query under
  the new workspace; any other page opens the new workspace's Projects.
- **New workspace** opens `/workspaces?customer_id=<space>`.

At phone width (`max-width: 40rem`) the top bar wraps: the actions keep the first row, and the
picker (two equal columns and *New workspace*) and the trail take a full-width row below, so a
390 px viewport shows whole space and workspace names. The taller bar is not sticky there, and
table headers stick to the top (`lake.css`).

## A workspace that is not ready

`components/WorkspaceGate.tsx` wraps Design, Train and Ship content. It reads `workspace_status`
and shows why the page is unavailable (no space, no workspace control plane, a failed status
read), that the workspace Zetta is being prepared (checked again every 15 seconds), that it
failed, or **Start** when it was never started (`start_workspace`). The Agent Library shows
whether or not the workspace is ready, because it is lake's, not the workspace's.

## The Workspaces page

`/workspaces` (`pages/workspaces/WorkspacesPage.tsx`) is outside `/w/` so the first workspace can be
created before one exists. Figures on top count the chosen customer's workspaces, the ready and
failed ones and the storage they request; the list shows each workspace's name, environment and
namespace, its phase as a state chip, its storage (requested, and observed when it differs) and its
age, with a search and a filter by phase. **New workspace** opens a dialog that creates one from a
name and an environment (`create_workspace`; `dev`, `staging`, `prod` or a lowercase DNS label). It
writes desired state; the operator provisions the namespace and Zetta.

**Delete** opens a confirmation dialog (`/workspaces?customer_id=…&delete=<workspace_id>`, so the
address still opens it). It first reads `delete_workspace {dry_run: true}` and lists what goes with
the workspace, with counts: its Zetta and every record in it, its repositories (live, and deleted
but still retained), its apps, its unfinished builds and its images, each by name under "Everything
by name". It says the deletion cannot be undone and asks for the workspace's name; a wrong name is
refused by the capability and shown on the field ([deletion](../api/workspaces.md#deletion),
lake#514). Cancel or Escape closes it and drops `delete=`.

## How to verify

- Open `/design/projects` signed in with one workspace: the address becomes
  `/w/<id>/design/projects`. With two workspaces and no remembered one, the page asks.
- In the Network panel, a call from `/w/<id>/train/data` goes to `/w/<id>/v1/…`, and a call from
  `/repositories` goes to `/v1/…`, after either order of visits.
- Switch workspace on `/w/<a>/train/runs?project=x`: the page reopens at `/w/<b>/train/runs?project=x`.
- Clear `localStorage` and reload a `/w/<id>/` page: it opens the same workspace.
- `HttpServer/HttpResponseIsolation.TheViewerSaysWhoIsSignedInAndWhereTheyAre` checks the `viewer`
  read's workspace validation.

## How to change

A page that works in a workspace sets `workspace: true` in its `routes.ts` and builds its links
with `workspacePath` (or the page group's `ws(...)` helper). Never read `lake-workspace` to decide
what a capability call is about; take the workspace from the URL. A new top-level path prefix that
holds workspace pages is added to `isWorkspacePath` in `routes.tsx`.

The storage rule, migration, retention and cross-repository follow-ups are in the
[workspace data-plane design](../architecture/workspace-data-plane.md).
