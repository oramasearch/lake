# Ship Apps pages

## What

`/w/<workspace_id>/ship/apps` (`frontend/src/pages/apps/AppsPage.tsx`) lists the space's apps
(`list_apps`) with name, URL, observed phase, mode, revision or image, and binding summary. It has
separate forms for runtime apps sourced from a lake repository (the repository choices come from
`list_repositories`) and for image apps, both `create_app`.

`/w/<workspace_id>/ship/app?app_id=<id>` (`AppPage.tsx`) is the operating page for one app
(`get_app`): desired and observed status, URL, deployment and rollback history, app and build logs,
masked environment references, Zetta and managed-service bindings, authentication, and the
confirmed delete control.

Every control calls its capability directly, as REST and MCP callers do, with the signed-in
person's authorization and audit. The page writes no app rows or operator state of its own.

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

Secret references render only as `••••••` and never appear in the page.

## Live state

The app page reads the retained `receive_app_events` history and then long-polls it (25-second
waits). The next sequence is kept per app in `localStorage["lake-app-events:<app>"]`, so opening the
page again resumes after the last event. Lake and the operator keep publishing to
`app-events.<app>` while no page is open. Event text is rendered as text, never as HTML.

Builds come from `list_builds`. Each build shows its retained `build_logs` and then follows
`receive_build_events` the same way, with its cursor in `localStorage["lake-build-events:<build>"]`.
App event panels link to the `app-events.<app-id>` topic and builds to `build-events.<build-id>` in
Train → Streams.

## ApplicationAgent

The Apps list checks the Agent Library (`list_agent_library`) for a deployed `agent-application`.
When it is available, **Build an App With the ApplicationAgent** starts an `agent_turn` with
`agent: agent-application` and `kind: app`, puts the task id in the URL (`?task=`), and follows the
task with `get_agent_task` every two seconds. The first turn has read tools only: workspace queries
and SQL, datasets, services, the space's repositories (`list_repositories`, `get_repository`, tree
and files) and apps. The repository reads let a confirmed retry find the repository its earlier
attempt created instead of creating a second one.

If the turn asks a question or has no proposal, the result has a **Reply** box. The page reads the
task again, checks that it still asks something in a conversation, and starts another turn in that
`conversation_id`.

A successful proposal shows the agent's reply, the App settings and every proposed file. **Confirm**
reads the task again and checks, in the browser, that it is still a succeeded ApplicationAgent `app`
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

In a workspace with ApplicationAgent deployed: build an app with the agent, reply to a question,
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
