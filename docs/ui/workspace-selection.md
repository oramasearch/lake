# Workspace Selection and Navigation

The top bar has one space selector and one workspace selector. Workspaces are non-deleted contract rows
of that space. A sole workspace is selected but remains visible; selection is sent explicitly.
Changing space refreshes its workspace choices. New workspace opens `/workspaces`, which lists
name, environment, phase, namespace and created time, and accepts name/environment for creation.
It writes desired state through `create_workspace`; the operator provisions the namespace and
Zetta. Delete requires typing the workspace name. Pending and stopped workspaces remain selectable.
IDs are `workspaces.id`, not service IDs. This management page is outside the `/w/` prefix so the
first workspace can be created before a selection exists.
Switching from Workspaces stays on that management page. Explicit space selection takes
precedence over a remembered workspace from another space on control-plane pages.

Design, Train, Ship, Ask and the workspace proxy use `/w/<workspace_id>/...`. Navigation ownership
is evaluated on the local page path, so `aria-current="page"` follows the selected page while
links retain the workspace prefix. Shared rendering carries the ID into URL attributes, inline
navigation scripts, and hidden form fields. It does not rewrite ordinary hidden field values.
Live app/build, Streams and Experiments requests send the same ID to REST explicitly.

The selection form submits to `/workspace/select`. Switching keeps the page and query filters,
removing old customer/workspace query values. Lake records `lake_workspace`, an HttpOnly,
SameSite=Lax navigation cookie. It is never an API default or an authorization credential.
Every remembered selection is checked against the caller's accessible spaces and workspace rows.
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
