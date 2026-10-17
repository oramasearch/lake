# Ship Apps pages

## What

`/w/<workspace_id>/ship/apps` (`frontend/src/pages/apps/AppsPage.tsx`) lists the account's apps
(`list_apps`) with name, URL, observed phase, mode, revision or image, and binding summary. It has
separate forms for runtime apps sourced from a lake repository (the repository choices come from
`list_repositories`) and for image apps, both `create_app`.

`/w/<workspace_id>/ship/app?app_id=<id>` (`AppPage.tsx`) is the operating page for one app
(`get_app`): desired and observed status, URL, deployment and rollback history, app and build logs,
masked environment references, Zetta and managed-service bindings, authentication, and the
confirmed delete control.

Every control calls its capability directly, as REST and MCP callers do, with the signed-in
person's authorization and audit. The page writes no app rows or operator state of its own.

## Layout (lake#561)

The pages follow the console's rich layout (`docs/ui/web-app.md`, *The look*):

- **Apps** opens with figures from `list_apps` (Apps, Ready, Failed, With Zetta), then a search and a
  phase filter over a card grid: each card shows the app's name, phase chip, URL, revision, bindings
  and when it was last updated (`updated_at`). **New app** opens a drawer with *From a repository* and
  *From an image*. **Build with the Application Agent** opens a drawer for the description. A task named
  by `?task=` is shown above the grid.
- **App** is a Split. The main column has tabs in `?tab=` (Overview, Builds, Logs, Events, Settings,
  Services), and every other query parameter is kept. Overview holds the deploy history with
  Rollback, which now asks once in a dialog, and the latest events. Logs and Events are dark
  terminal panels. The side column has the rollout status, Scale, Deploy with Restart, Stop and
  Start, and the typed-name delete.
- **Packages** has a tab per registry kind (Container Images, npm, Python) and a Builds tab. Each
  package is a card with its versions as chips and its install command in a copy block.
- **Deployments** and a repository's **Environments** show figures, a search and a phase filter,
  and rows with the phase as a state chip and relative times. **Environment** is a Split.
  Deleting a domain now asks once. Deployments reads `server_info` first: on a lake whose
  `deployments` is `false` it shows "Deployments Are Not Set Up on This Lake" after that one call
  and asks no repository (lake#605, `apps/deployments.ts`,
  [platform.md](../api/platform.md#a-lake-without-the-platform-mirror)).
- **Logs** follows in a terminal panel.

`apps/shared.tsx` holds the group's parts: `QueryTabs`/`useQueryTab`, `Terminal` and
`LiveSwitch`. Their styles are in `styles/ship.css`, along with `form.row-form` (compact forms whose
labels stack over their controls).

## Controls

| Control | Capability |
|---|---|
| Restart, Stop, Start | `restart_app`, `stop_app`, `start_app` |
| Scale | `scale_app` |
| Deploy, Rollback (to a revision in the history) | `deploy_app`, `rollback_app` |
| Build, Cancel build, Deploy build | `build_app`, `cancel_build`, `deploy_app` |
| Set or unset an environment variable, plain or Secret-referenced | `set_app_env`, `unset_app_env` |
| Attach or detach the workspace Zetta | `set_app_zetta_binding` |
| Attach or detach an existing service; create and attach one | `bind_service_to_app`, `unbind_service_from_app`, `create_service_and_bind_to_app` (choices from `list_services`) |
| Authentication | `set_app_auth`; public access needs its own *Confirm public access* checkbox |
| Delete | `delete_app`; requires the app's exact name |
| Logs | `app_logs` (its `items`) |

A secret variable renders as the secret and field it comes from (`database → DATABASE_URL`,
`secretEnvLabel` in `frontend/src/pages/apps/secretEnv.ts`); its value never reaches the page. The
Environment form sets either a value or a secret (**Secret mount ID**, **Secret name**, **Field**;
`envInput`), lake#625.

## Live state

The app page reads the retained `receive_app_events` history and then long-polls it (25-second
waits). The next sequence is kept per app in `localStorage["lake-app-events:<app>"]`, so opening the
page again resumes after the last event. Lake and the operator keep publishing to
`app-events.<app>` while no page is open. Event text is rendered as text, never as HTML.

Builds come from `list_builds`. Each build shows its retained `build_logs` and then follows
`receive_build_events` the same way, with its cursor in `localStorage["lake-build-events:<build>"]`.
App event panels link to the `app-events.<app-id>` topic and builds to `build-events.<build-id>` in
Train → Streams.

## Application Agent

The Apps list checks the Agent Library (`list_agent_library`) for a deployed `agent-application`.
When it is available, **Build an App With the Application Agent** starts an `agent_turn` with
`agent: agent-application` and `kind: app`, puts the task id in the URL (`?task=`), and follows the
task with `get_agent_task` every two seconds. The first turn has read tools only: workspace queries
and SQL, datasets, services, the account's repositories (`list_repositories`, `get_repository`, tree
and files) and apps. The repository reads let a confirmed retry find the repository its earlier
attempt created instead of creating a second one.

If the turn asks a question or has no proposal, the result has a **Reply** box. The page reads the
task again, checks that it still asks something in a conversation, and starts another turn in that
`conversation_id`.

A successful proposal shows the agent's reply, the App settings and every proposed file. **Confirm**
reads the task again and checks, in the browser, that it is still a succeeded Application Agent `app`
proposal asking for confirmation and that it is the same proposal that was shown (its JSON with
sorted keys compared to what is on screen); otherwise nothing starts. It then starts a second turn
with the proposal at `context.confirmed_proposal`. Only a turn carrying a well-formed confirmed
proposal receives repository writes plus `create_app`, `deploy_app` and the build chain
(`build_app`, `get_build`, `receive_build_events`, `build_logs`) for apps whose dependencies need a
built image ([../api/web-apps.md](../api/web-apps.md)). A deployed result shows its URL.

**What changed (lake#561).** The server's confirm form recomputed the proposal's SHA-256 against
the stored task before starting the turn. That check now runs in the page against a fresh
`get_agent_task` read. `agent_turn` itself grants the write tools to any turn whose
`confirmed_proposal` has the proposal's shape, as it always did for a REST or MCP caller.

A proposal whose app has public access (`auth` `none` or `public`) shows **Access: Public — no
sign-in**, a warning that anyone with the app's address can open it, and a required *I understand
this app will be public* checkbox (lake#452). Confirming without it starts no turn; with it, the
confirmed turn carries `context.confirm_public: true`, which `create_app` requires before it creates
a public app for the agent.

## Verification

In a workspace with the Application Agent deployed: build an app with the agent, reply to a question,
confirm the proposal (and a public one with and without the checkbox), then on the app page
restart, scale, set and unset a Secret-referenced variable (it shows as `••••••`), attach and
detach Zetta and a service, change authentication, follow a build's live log, reload the page and
see the events resume, and delete with a wrong and then the right name.

The capability contract and its tests are in [../api/web-apps.md](../api/web-apps.md) and
[../api/builds.md](../api/builds.md).

## Change

A new control calls an existing capability, or a new one registered first. Keep the proposal
checks before the confirmed `agent_turn`; any check the server must enforce belongs in the
capability, not the page.
