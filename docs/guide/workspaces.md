# Workspaces

A workspace is an isolated environment with an engine of its own. Your datasets,
projects, pipelines, runs, models, search indexes, transcripts, topics and Ask Orama conversations
live in it, sealed with the workspace's own key and kept apart from every other customer's. Your
account can have many workspaces, for example one per environment, and every Design, Train and Ship
page works in exactly one of them.

## Concepts

- **The workspace is in the address.** A workspace page answers at `/w/<workspace id>/…`, such as
  `/w/<workspace id>/design/projects`. You can bookmark it, share it, or open two workspaces in two
  tabs. An address without `/w/` (from an old link) opens the same page in your current workspace.
- **Every call names it.** Design, Train and Ship calls take `customer_id` (the id of the account
  that owns your workspaces: yours, or your organization's) and `workspace_id`. A call without
  `workspace_id` is refused; Lake never picks a default.
- **Desired state and observed phase.** Creating, stopping or deleting a workspace records what you
  asked for. The Orama operator then provisions or removes the workspace's engine and reports its phase
  (for example `Ready` or `Failed`). A workspace is *ready* when the operator reports it ready and
  Lake can reach it.
- **What stays outside a workspace.** Repositories, organizations, your account pages, Services,
  Deployments and Workspaces itself are outside `/w/`. A repository still belongs to one workspace,
  chosen when it is created, and its page names it.

## Choose a Workspace

The pill in the top bar names the current workspace.

1. Press it. The menu lists **Your workspaces**, then each organization's workspaces under the
   organization's name.
2. Choose a workspace. A workspace page reopens at the same place in the new workspace (with the same
   filters); any other page opens the new workspace's **Projects**.
3. **New workspace** under a group opens **Ship → Workspaces** for that account.

When the address names no workspace and none is remembered, the page asks you to choose one. The
browser remembers the last workspace you opened, only to decide where an unprefixed link goes; it
never decides what a call does.

- REST: `GET /v1/list_workspaces?customer_id=<customer id>` lists them; each `id` is a
  `workspace_id`.
- MCP: `list_workspaces` with the same input.

## Start Your First Workspace

On any Design, Train or Ship page of an account with no workspace, the page shows **Start Your
Workspace**.

1. Press **Start your workspace**.
2. The page shows *Preparing Your Workspace* with the operator's phase, and checks again every 15
   seconds.
3. When it is ready, the page you were on opens.

The first workspace is named `workspace`, in the `prod` environment. Starting again is harmless.

- REST: `POST /v1/start_workspace` with `{"customer_id": "<customer id>"}`. The answer carries its
  `workspace_id`.
- MCP: `start_workspace` with the same input.

## Create a Workspace

1. Open **Ship → Workspaces** and press **New workspace**.
2. Enter a **Name** (up to 128 characters, unique among your live workspaces) and an
   **Environment**: `dev`, `staging`, `prod` or another lowercase DNS label (letters, digits and
   `-`, at most 47 characters).
3. Press **New workspace**. The list shows it with its phase until it is `Ready`.

- REST: `POST /v1/create_workspace` with this body:

```json
{"customer_id": "<customer id>", "name": "analytics", "environment": "dev"}
```

- MCP: `create_workspace` with the same input. The answer's `id` is the new `workspace_id`.

`environment` says what the workspace is for. Use `dev`, `staging` or `prod`, or any lowercase
DNS label of up to 47 characters (letters, digits and inner hyphens, such as `qa` or `eu-prod`). It
names the workspace's namespace on the cluster and changes nothing else, so it is a label rather
than a fixed list.

## Check Whether It Is Ready

**Ship → Workspaces** shows every workspace's name, environment, phase, storage and age, with a
search and a filter by phase, and figures for how many are ready or failed and the storage they
request.

- REST: `GET /v1/workspace_status?customer_id=<customer id>&workspace_id=<workspace id>` answers
  `requested`, `phase`, `message` and `ready`.
- REST: `GET /v1/get_workspace?customer_id=<customer id>&workspace_id=<workspace id>` answers the
  name, environment, desired state and the operator's observations, including `storage_gi`.
- MCP: `workspace_status` and `get_workspace` with the same inputs.

### What the Pages Show Until It Is Ready

Every Design, Train and Ship page waits for its workspace:

| The page says | What it means |
|---|---|
| **Start Your Workspace** | No workspace was ever requested. Press **Start your workspace**. |
| **Preparing Your Workspace** | Requested; the operator is provisioning it. The page checks every 15 seconds. |
| **Your Workspace Could Not Be Prepared** | The operator reported `Failed`, with its reason. *workspace quota reached* means this Lake already runs as many workspaces as it allows; an operator can raise the limit. **Cancel workspace** opens Workspaces so you can withdraw it. |
| **Workspace Unavailable** | The status could not be read; the reason is shown. |
| **Not Available on This Lake** | This Lake has no workspace control plane, so Design, Train and Ship are not available on it. |
| **No Workspace Yet** | You have no account to hold workspaces yet. |

The **Agent Library** opens whether the workspace is ready or not, because it is Lake's, not the
workspace's.

## Rename, Stop and Start a Workspace

There is no button for this on the site; use the API. Stopping withdraws the workspace's engine
and keeps its data; starting brings it back.

Through the REST API, send `POST /v1/update_workspace` with this body:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "desired": "stopped"}
```

Send `desired` as `running` to start it again, or a new `name` or `environment` to rename it.
Through MCP, call `update_workspace` with the same input.

## Storage Size

Each workspace's volumes have a desired size, `storage_gi`, 5 GiB by default. **Ship →
Workspaces** shows the size requested and, when it differs, the size the operator observed. Only
the operator or the instance administrator changes it, and a size never goes down. Ask your Lake's
operator for more room. How much your account stores in all is on **Organization → Usage**
([Usage and Metering](/docs/usage)).

## Delete a Workspace

Deleting a workspace destroys everything in it and **cannot be undone**: retained volume snapshots
are the only copy left.

1. Open **Ship → Workspaces** and press **Delete** on the workspace's row.
2. The dialog lists what goes, with counts:
   - its engine, with every record, dataset, topic and store in it, and its own audit history;
   - its repositories, live ones and deleted ones still retained (their retention ends now and their
     names become free);
   - its apps;
   - its unfinished builds, cancelled;
   - its image repositories, no longer pullable.
   **Everything by name** lists each one.
3. Type the workspace's name under **Type `<name>` to confirm** and press **Delete workspace and
   everything in it**. A wrong name is refused and nothing changes.

The row then reads *Deletion requested* until the operator reports it deleted. Lake records one
audit event per destroyed repository, app, build and image in the central log, so the history
survives the workspace.

Through the REST API, send `POST /v1/delete_workspace` twice. First a dry run, which deletes
nothing and answers what would go, with `counts`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "dry_run": true}
```

Then the deletion itself, with `confirm` equal to the workspace's name:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "confirm": "analytics"}
```

Through MCP, call `delete_workspace` with the same inputs.

If Lake stops part way, the workspace is still deleted and `deletion.completed` is `false` with the
reason in `deletion.pending`; Lake finishes it on its own, and calling `delete_workspace` again
resumes it.

## Who Can Use a Workspace

A workspace belongs to an account, and access comes from that account:

| To | You need on the account |
|---|---|
| Open Design, Train and Ship pages, read data, run read-only SQL | `read` |
| Create projects, load data, run pipelines, create and stop workspaces | `write` |
| Delete a workspace | `admin` |

In an organization, `owner` and `admin` members administer its workspaces and `member` members read
them; an administrator grants `write` to people who need to change things ([Organizations, Members
and Teams](/docs/organizations), [Grants and Effective Access](/docs/access)). Your own account's
workspaces are yours alone unless you grant someone access.

## Limits

- Workspace reads time out after 15 seconds and actions after 60.
- A Lake may cap how many workspaces and services an account runs; the operator's refusal names the
  limit.
- Names are unique among an account's live workspaces.
- Stopping and renaming are API-only.

## Troubleshooting

| Error | What to do |
|---|---|
| `workspace_not_ready` (503) | The workspace is not ready yet. Poll `workspace_status`. |
| `invalid_input` at `/workspace_id` | The id is unknown or is not a workspace of this account. Check `list_workspaces`. |
| `invalid_input` at `/customer_id` | The `customer_id` is not the account that owns this workspace. |
| `invalid_input` at `/environment` | Use `dev`, `staging`, `prod` or a lowercase DNS label. |
| `invalid_input` at `/confirm` | The name typed for deletion does not match. |
| `unauthorized` (403) | You lack the role above on the account. |
| `store_unavailable` (503) | This Lake has no workspace control plane, or it cannot be reached now. |
| A page under `/w/<id>/` answers that the id does not match | A call named a different workspace than the address; open the page from the picker. |

## Related Pages

- [Getting Started](/docs)
- [Concepts and Glossary](/docs/concepts)
- [Organizations, Members and Teams](/docs/organizations)
- [Services and Storage](/docs/services)
- [Usage and Metering](/docs/usage)
- [Security Model](/docs/security)
