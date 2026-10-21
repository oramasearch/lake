# Apps

**Ship → Apps** runs your own web applications on Lake: a dashboard over a dataset, an internal
tool, a small API. An app runs from a Lake repository or from a container image, gets its own
address, and by default asks people to sign in to Lake before they reach it. You can build one
yourself, or describe it to the Application Agent, review exactly what it proposes, and confirm.

Every call on this page names `customer_id`, the id of the account that owns your workspaces (yours,
or your organization's), and `workspace_id`, the workspace the app belongs to. An app never moves
to another workspace.

## Concepts

| Term | What it is |
|---|---|
| **App** | Desired state that Lake records and the Orama operator runs: a name, a mode, a source or image, a framework and a port, plus replicas, access, bindings and environment. |
| **Runtime app** (*From a repository*) | Runs a revision of a Lake repository on Lake's app runtime. The source is `{repository, revision, path, entrypoint}`; with no entrypoint the runtime reads the app's `app.toml`. |
| **Image app** (*From an image*) | Runs a container image you name. |
| **Address** | `https://<name>.<the lake's app domain>` unless a host is given. On lake.oramadev.net an app named `reports` is `reports.oramadev.net`. |
| **Access** | **Lake Sign-In** (the default): only members of the app's workspace reach it, after signing in to Lake. **Public**: anyone with the address, which needs an explicit confirmation. |
| **Bindings** | Read-only access to the workspace's data, and connections to services of your account. |
| **Generation** | Every change to an app's desired state increases its generation; the operator reports the generation it has applied. |

## Ship → Apps

The page opens with figures (**Apps**, **Ready**, **Failed**, **With Workspace Data**), then **Search apps…**
and filter chips by phase over a card per app: its name, phase, address, revision or image, its
bindings and when it last changed. **Open** on a card opens the app.

### Create an App from a Repository

1. Open **Ship → Apps** and press **New app**.
2. Keep **From a repository**.
3. Fill in **Name** (lowercase letters, digits and `-`, starting with a letter), choose the
   **Repository**, and set **Revision** (default `main`), **Path** (default `.`), **Framework**
   (default `auto`) and **Port** (default 3000).
4. Press **Create app**. The app's page opens.

- REST: `POST /v1/create_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "reports", "mode": "runtime", "framework": "auto", "port": 3000, "source": {"repository": "<repository id>", "revision": "main", "path": "."}}
```

- MCP: `create_app` with the same input.

### Create an App from an Image

1. **Ship → Apps → New app**, then choose **From an image**.
2. Fill in **Name**, **Image**, pinned by digest (`registry.example/app@sha256:<64 hex digits>`;
   a tag such as `app:1.27` is refused, because the operator runs only a digest-pinned image),
   **Framework** (default `container`) and **Port**.
3. Press **Create app**.

- REST: `POST /v1/create_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "tracker", "mode": "image", "image": "<registry host>/<handle>/tracker@sha256:<digest>", "framework": "container", "port": 8080}
```

- MCP: `create_app` with the same input.

`create_app` also takes `replicas` (1 to 100), `host`, `auth` (`lake`, `none` or `public`),
`bindings` and `env`. An app created public needs `confirm_public: true`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "status-board", "mode": "image", "image": "<image>", "framework": "container", "port": 8080, "auth": "none", "confirm_public": true}
```

An image you built in Lake is named on [Packages, Builds and the Registry](/docs/registry).

## Build an App with the Application Agent

Application Agent writes the app for you: it reads what it needs, proposes the repository files and
the app settings, and changes nothing until you confirm.

1. Open **Ship → Apps** and press **Build with the Application Agent**.
2. Under **Description**, say what the app should do, for example *a page listing the accounts most
   at risk this week from the churn_scores dataset*. Press **Start the Application Agent**.
3. The page shows **Application Agent Is Working** and checks again every 2 seconds. You can leave;
   the turn keeps running and the page address (`?task=`) brings you back to it.
4. **The first turn cannot write anything.** It may only read: your datasets and their schemas,
   read-only SQL results, your services, your repositories and your existing apps.
5. When it asks a question instead of proposing, answer in **Reply** and press **Reply to
   Application Agent**; the conversation continues.
6. When it proposes, the **Proposed App** card shows the agent's explanation, the app settings
   (**Mode**, **Framework**, **Access**, **Repository**, **Path**) and **every file** it would commit,
   in full. Review all of it.
7. If **Access** reads **Public — no sign-in**, the page warns that anyone with the app's address
   can open it, and you must tick **I understand this app will be public** first.
8. Press **Confirm and deploy**. Lake reads the task again and checks that the proposal is still the
   one on your screen; if it changed, nothing starts. Only then does a **second turn** start, with
   that exact proposal. Only this turn may create the private repository, commit its files, create
   the app and deploy it. The result shows the agent's reply and **Open the app**.

The same through REST and MCP is two `agent_turn` calls.

- The proposing turn, REST `POST /v1/agent_turn`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-application", "kind": "app", "message": "A page listing the accounts most at risk this week from churn_scores."}
```

- Follow its `task_id` with `get_agent_task` until it finishes. Its answer's `result` is the proposal
  (`kind: proposal`, `confirm: true`, `files`, `app`), and `proposal_sha256` is its digest.
- After reviewing it, the confirming turn, REST `POST /v1/agent_turn` again, names the proposing
  turn and the digest you reviewed, and carries the proposal unchanged at
  `context.confirmed_proposal` (for a public app, `context` also carries `confirm_public` set to
  true):

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-application", "kind": "app", "message": "Confirm and deploy the reviewed proposal.", "proposal_task_id": "<task id>", "proposal_sha256": "<proposal_sha256 from get_agent_task>", "context": {"confirmed_proposal": {"kind": "proposal", "confirm": true, "files": [], "app": {"name": "risk-board"}}}}
```

- MCP: the `agent_turn` tool, with the same inputs.

Lake compares `proposal_task_id`, `proposal_sha256` and `confirmed_proposal` with the proposal it
stored; a different proposal is refused (*The Application Agent proposal changed before
confirmation.*). You can also choose the Application Agent in **Ask Orama → Talk to**, or ask Ask Orama to
hand an app request to it; the review and confirmation happen the same way before anything is
written.

If the Application Agent is not running on your Lake, the drawer says *The Application Agent is not
running on this Lake.* and cannot start.

## Operate an App

Open the app from its card. The header shows its name, phase and address, with **Open app** and
**Event stream** (its events in [Streams](/docs/streams)). The side column holds **Rollout** (phase,
desired state, generation and the observed generation, address, mode, framework, port, access),
**Replicas**, **Deploy and Lifecycle** and **Delete App**. The tabs are **Overview**, **Builds**,
**Logs**, **Events**, **Settings** and **Services**.

### Deploy a New Revision or Image

In **Deploy and Lifecycle**, set **Deploy revision** (a runtime app) or **Deploy image** (an image
app) and press **Deploy**.

- REST: `POST /v1/deploy_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "source": {"repository": "<repository id>", "revision": "<change or branch>", "path": "."}}
```

- For an image app send `image` instead of `source`. MCP: `deploy_app` with the same input.

### Roll Back

**Overview** shows **Deploy History**: each create, deploy and rollback with its generation, who did
it and what it deployed. Press **Rollback** on an earlier row and confirm **Roll back**: the app is
deployed again at that revision or image.

- REST: `POST /v1/rollback_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "image": "<image from the history>"}
```

- MCP: `rollback_app` with the same input (or `source` for a runtime app).

### Restart, Stop and Start

**Restart**, **Stop** and **Start** in **Deploy and Lifecycle**. A stopped app runs no replicas but
keeps its settings and its replica count; **Start** restores that count.

- REST: `POST /v1/restart_app`, `/v1/stop_app` or `/v1/start_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>"}
```

- MCP: `restart_app`, `stop_app`, `start_app`.

### Scale

Set **Replicas** (1 to 100) and press **Scale**.

- REST: `POST /v1/scale_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "replicas": 3}
```

- MCP: `scale_app` with the same input.

### Build the App's Image

For a runtime app whose dependencies need an image, open **Builds** and press **Build** (then
**Rebuild**). Each build shows its phase and revision, its log under **Logs**, **Deploy build**
once it produced an image, **Cancel build**, and **Browse build stream**.

- REST: `POST /v1/build_app` (here the app is named `app`):

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app": "<app id>"}
```

- MCP: `build_app`. Once the app runs a built image, pass `source` (`repository`, `revision`,
  optional `path`) to build again. Builds are described on
  [Packages, Builds and the Registry](/docs/registry#build-an-apps-image).

### Logs and Events

- **Logs** shows the last 200 log lines of the app's containers, with **Refresh**.
- **Events** follows the app's lifecycle, rollout and pod events live; **Browse app event stream**
  opens its topic `app-events.<app id>` in Train → Streams. Opening the page again resumes after the
  last event it showed.

- REST: `GET /v1/app_logs?customer_id=<customer id>&workspace_id=<workspace id>&app_id=<app id>&limit=200`
- REST: `GET /v1/receive_app_events?customer_id=<customer id>&workspace_id=<workspace id>&app_id=<app id>&from_sequence=1`
- MCP: `app_logs` and `receive_app_events`.

### Environment Variables and Secret References

**Settings → Environment** lists the app's variables; a secret shows as the secret and field it
comes from, never its value. To add one, fill **Key** and either **Value**, or, for a credential,
**Secret mount ID**, **Secret name** and **Field**: a field of a secret you keep in a secret mount of
this workspace ([Secrets and Encryption Keys](/docs/secrets)). Press **Set**. **Unset** removes one.
The app receives the secret when it is deployed, and again, with a restart, within five minutes of
you putting a new version of it.

- REST: `POST /v1/set_app_env`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "key": "REPORT_TITLE", "value": "Weekly risk"}
```

- A credential goes in as a field of a secret kept in a secret mount of the app's workspace, never
  as a value. Lake checks that the secret has the field, so put the secret in the mount first:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "key": "CRM_TOKEN", "secret": {"mount_id": "<mount id>", "name": "crm", "field": "token"}}
```

- `POST /v1/unset_app_env` takes `app_id` and `key`. MCP: `set_app_env`, `unset_app_env`.

Lake scans every plain value; one that looks like a credential is refused with `secret_required`.
The audit records the key and whether it is secret, never the value.

### Authentication

**Settings → Authentication**: choose **Lake Sign-In** or **Public** under **Access** and press
**Change authentication**. Public needs **Confirm public access** ticked.

- REST: `POST /v1/set_app_auth`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "auth": "none", "confirm_public": true}
```

- MCP: `set_app_auth`. Without `confirm_public` it is `invalid_input` at `/confirm_public`.

With **Lake Sign-In**, a visitor who is not signed in is sent to Lake's sign-in and back to the app.
The app receives the person in the `X-Orama-User` header and never sees their Lake session. The app's
own cookie lasts 12 hours, and Lake checks on every request that the person still belongs to the
workspace, so removing someone from the workspace takes effect at once.

With **Public**, nobody signs in, so the app has no signed-in person: `current_user()` from
`orama_app` returns `None`. Anyone can send an `X-Orama-User` header (or any other identity header) to
a public app, so an app that decides who sees what must use **Lake Sign-In**. An app in image mode
that reads these headers itself must not treat them as identity unless it uses **Lake Sign-In**; see
[Which identity headers an app may trust](../api/web-apps.md#lake-protected-app-authentication).

### Bindings: Workspace Data and Services

**Services** tab:

- **Workspace data**: **Attach workspace data** gives the app read-only access to the workspace's
  data; **Detach workspace data** takes it away at once.
- **Services**: choose a **Service** and press **Attach service**; **Detach** removes one.
- **Create and Attach**: fill **Name**, **Kind** (for example `postgres`) and **Size** in GiB and
  press **Create and attach** to create a new service and bind it in one step.

- REST: `POST /v1/set_app_zetta_binding`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "attached": true}
```

- REST: `POST /v1/bind_service_to_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "service_id": "<service id>"}
```

- REST: `POST /v1/create_service_and_bind_to_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "kind": "postgres", "name": "reports-db", "storage_gi": 5}
```

- MCP: `set_app_zetta_binding`, `bind_service_to_app`, `unbind_service_from_app`,
  `create_service_and_bind_to_app`.

Connections reach the app as mounted Secrets; Lake never puts a token into the app's code or
environment.

### Delete an App

In **Delete App**, type the app's name under **App name** and press **Delete app**. The app shows
*deleted* as its desired state until the operator has removed it.

- REST: `POST /v1/delete_app`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "app_id": "<app id>", "confirm": "reports"}
```

- MCP: `delete_app`. A `confirm` that is not the app's exact name deletes nothing.

### Read Apps from Code

- REST: `GET /v1/list_apps?customer_id=<customer id>&workspace_id=<workspace id>`
- REST: `GET /v1/get_app?customer_id=<customer id>&workspace_id=<workspace id>&app_id=<app id>`
- MCP: `list_apps`, `get_app`. Each app carries its desired and observed state: `phase`, `url`,
  `ready`, `message` and `observed_generation`.

## Limits

- Names are DNS labels. These are reserved and refused (`<name> is reserved` at `/name`): `lake`,
  `www`, `api`, `app`, `apps`, `admin`, `mcp`, `docs`, `status`, `mail`, `smtp`, `auth`, `login`,
  `sso`, `id`, `cdn`, `static`, `assets`, `grafana`, `prometheus`, `argocd`, `registry`, `git`,
  `dev`, `staging`, `prod`, `test`, and every name starting with `lake-` or `orama`. A Lake's operator
  may reserve more.
- A host must be one label below the Lake's app domain.
- Replicas are 1 to 100 per app. A Lake's operator may cap the number of apps and the replicas per
  account; past the cap the call is refused.
- An app reads the workspace's data only, never writes it, and only when workspace data is attached.
- App events are kept for 24 hours, up to 5,000 per app.
- **Logs** shows the last 200 lines; `app_logs` answers at most 1,000 per call.

## Permissions

| Step | Needs |
|---|---|
| See apps, logs and events | `read` on the account that owns the workspace |
| Create, deploy, roll back, restart, stop, start, scale, set environment, authentication, bindings, build, delete | `write` on the account |
| Reach a Lake Sign-In app | membership of the app's workspace |
| Confirm an Application Agent proposal | the person following the proposing turn |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| `invalid_input` at `/name`, *is reserved* | The name is one of the reserved names. | Choose another name. |
| `invalid_input` at `/confirm_public` | Public access was asked for without confirming it. | Tick the confirmation, or send `confirm_public: true`. |
| `secret_required` | A plain environment value looks like a credential. | Put it in a secret mount of the workspace ([Secrets and Encryption Keys](/docs/secrets)) and set the variable from that secret's field. |
| `conflict` | An app with that name already exists. | Choose another name. |
| `invalid_input` at `/workspace_id` | The workspace is missing, unknown, or not one of the account's. | Use the workspace in the page address. |
| *That task has no Application Agent proposal to confirm.* | The turn asked a question or failed, or is not an app turn. | Reply, or start a new turn. |
| *The Application Agent proposal changed before confirmation.* | What you confirmed is not what the turn proposed. | Review the proposal shown now and confirm that. |
| The phase stays *Failed* | The operator could not run it; **Rollout** shows its message. | Read **Logs** and **Events**, fix the source or image, and **Deploy** again. |
| *Logs are not available on this Lake yet.* | This Lake has no log source configured. | Use **Events**, or ask the Lake's operator. |

## Related Pages

- [Packages, Builds and the Registry](/docs/registry): build images and name them in an app.
- [Services and Storage](/docs/services): the databases an app binds.
- [Streams](/docs/streams): an app's events as a topic.
- [Ask Orama](/docs/ask-orama): asking for an app in conversation.
- [Agents](/docs/agents): Application Agent in the Agent Library.
- [Repositories](/docs/repositories): where a runtime app's source lives.
- [Secrets and Encryption Keys](/docs/secrets): references for credentials.
