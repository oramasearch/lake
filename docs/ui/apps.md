# Ship Apps pages

## What

`GET /ship/apps` lists the selected customer's apps with name, URL, observed phase, mode,
revision or image, and binding summary. It also has separate forms for runtime apps sourced from
a Lake repository and image apps. `GET /ship/app?customer_id=<id>&app_id=<id>` is the operating
page for one app: desired and observed status, URL, deployment and rollback history, app and build
logs, masked environment references, Zetta and managed-service bindings, authentication, and the
confirmed delete control.

All forms are adapters over the customer-scoped app capabilities. They retain the signed-in
principal, registry policy, audit record, and `x-lake-action` classification used by REST and MCP.
The page does not write app rows or operator state directly.

## Live state

The app page starts at the retained `receive_app_events` history and long-polls that capability
through its REST route. The next sequence is kept per app in browser local storage, so opening the
page again resumes after the last event. Lake and the operator continue publishing to
`app-events.<app>` while no page is open. The live log uses text nodes and never treats event data
as HTML.

When the independently delivered build capabilities are present, the page calls `list_builds`,
`get_build`, and `build_logs`, offers `build_app` and `cancel_build`, and follows each
`receive_build_events` stream with the same cursor behavior. An older lake answers
`unknown_capability`; the page then says builds are unavailable and every app control still works.

## ApplicationAgent

The Apps list checks the Agent Library for the deployed `agent-application` registration. When it
is available, **Build an App With the ApplicationAgent** starts an `agent_turn` with `agent:
agent-application` and `kind: app`, redirects with the task id, and follows the background turn.
The first turn has read tools only: workspace queries and SQL, datasets, services, the
customer's repositories (`list_repositories`, `get_repository`, tree and files) and apps. The
repository reads let a confirmed retry find the repository its earlier attempt created instead
of creating a second one.

If the turn asks a question or has no proposal, the result includes a **Reply** box. Its source
task ID selects the same ApplicationAgent `app` conversation and supplies the preceding turn to
the follow-up. Lake rejects tasks belonging to another person, space, agent or kind, and rejects a
reply after a proposal is ready for confirmation.

A successful proposal renders the agent's reply, App settings, and every proposed file. The
confirmation form carries the SHA-256 of the exact stored proposal. Lake reloads the task for the
same customer, verifies that it is a succeeded ApplicationAgent `app` task and that the digest is
unchanged, then starts a second turn with the proposal at `context.confirmed_proposal`. Only that
turn receives repository writes plus `create_app`, `deploy_app`, and the build chain (`build_app`, `get_build`, `receive_build_events`, `build_logs`) for apps whose dependencies need a built image. A changed digest, another kind
of task, or a proposal that did not request confirmation is refused. A deployed result shows its
URL.

## Controls

The app page exposes restart, scale, stop/start, deploy and history-bound rollback, build/cancel,
plain or Secret-referenced environment updates, Zetta attach/detach, existing-service
attach/detach, create-and-attach service, and authentication changes. Public authentication
requires its separate confirmation checkbox. Delete requires the exact app name. Secret
references render only as `••••••` and are not included in page source.

## Verification

`WebUi.ShipAppsRenderControlsMaskedSecretsAndLiveEvents` verifies the pure renderers, masking,
history-bound rollback, build history, and both polling endpoints. The four
`HttpServer/HttpResponseIsolation.ShipAppsCreatesOperatesAndFollowsAnApp/*` variants exercise the
ApplicationAgent start, immutable proposal confirmation and deployed result, then the ordinary App
routes, with memory and Postgres stores; the memory variants add fake operator status.
`tools/ui/test-token-gates.sh` checks that the added styles use only design tokens.
