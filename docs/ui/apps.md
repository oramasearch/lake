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

The Apps list checks the Agent Library for the deployed `agent-application` registration. Until
that registration is delivered, the description box and its submit button are disabled and the
page explains why. Once registered, the form starts an `agent_turn` with `agent:
agent-application` and `kind: app`; proposal review and confirmation are owned by that agent's
registration contract.

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
real HTTP routes with memory and Postgres stores; the memory variants add fake operator status.
`tools/ui/test-token-gates.sh` checks that the added styles use only design tokens.
