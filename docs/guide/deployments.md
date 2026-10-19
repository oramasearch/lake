# Deployments and Environments

A repository can be deployed to named **environments**, such as `production` or `staging`, with
liquid's `liquid deploy` command. The Orama operator runs each deployment and reports its phase;
Lake mirrors what the operator reports, so you can see what is running, which change it runs and
where it is served, and change an environment's configuration and domains.

**Ship → Deployments** shows every environment across the repositories you can open. Each
repository's own **Deployments** tab shows its environments, and each environment has its own page
with its current deployment, history, variables, domains and logs.

These pages describe deployments made with `liquid deploy`. Ship → Apps is a separate way to run
an app ([Apps](/docs/apps)).

## Concepts

| Term | What it is |
|---|---|
| **Environment** | A named place a repository is deployed to. It belongs to the repository, and the repository's workspace holds its records. |
| **Deployment** | One request to run a change or an image in an environment, with the phase the operator reports, its namespace and its address. |
| **Environment variable** | A plain configuration value of one environment. Credentials are not environment variables. |
| **Domain** | A host name that serves an environment, with its TLS certificate from ACME or provided by you. |
| **Stale** | When Lake cannot reach the operator's control plane, the pages keep showing the last state Lake mirrored and say since when it is stale. |

## Deploy with Liquid

Deployments are created from a checkout with liquid, not from the web pages:

```
liquid deploy --env staging --image <lake host>/<handle>/reports@sha256:<digest>
liquid deploy status --env staging
liquid deploy list --env staging
```

- `--env` names the environment (default `production`); `--image` deploys an image reference by
  digest; `--wait` fails when the new deployment does not become ready.
- **Roll back** with `liquid deploy --to <change or deployment>`: a rollback is another deployment of
  an earlier change or deployment.

An image built in Lake is named on [Packages, Builds and the Registry](/docs/registry). The
environment appears in Lake once the operator has reported it; Lake checks the operator every 15
seconds.

Run `liquid deploy` in a checkout cloned from the repository in Lake (`liquid clone`), or in the
checkout you pushed to a new, empty repository: liquid names the repository by an id the checkout
shares with it, and Lake shows the deployment on that repository. A deployment Lake cannot place
on one repository is not shown, and Lake reports it to the people who run it.

## Ship → Deployments

**Ship → Deployments** opens with figures (**Environments**, **Repositories** with an environment,
**Healthy**, **Failing**), then **Search environments…** and a filter by phase over one row per
environment: its repository and name, phase, the change and image it runs, its address and when it
last changed. With nothing deployed, the page says *Nothing Deployed Yet*.

## A Repository's Environments

1. Open the repository (**Data → Repositories**, then the repository).
2. Open the **Deployments** tab. It lists the repository's environments with the same figures,
   search and phase filter.
3. Open an environment.

- REST: `GET /v1/list_environments?repository_id=<repository id>`
- REST: `GET /v1/list_deployments?repository_id=<repository id>&environment=production&limit=20`
- MCP: `list_environments`, `list_deployments`.

Each environment summary carries its id, current phase, change, image, address, domains and update
time. `list_deployments` answers the latest record of each deployment, newest first.

## An Environment's Page

The environment page shows:

- **Current Deployment**: its phase, change, image, namespace and address.
- **Container Logs**, with **View logs**.
- **Environment Variables**, with **Set variable** and **Unset**.
- **Domains**, with **Create domain** and **Delete**.
- **Deployment History**: every deployment of the environment, newest first.

- REST: `GET /v1/get_environment?repository_id=<repository id>&environment=production`
- MCP: `get_environment`. The answer has the current deployment, the history, the domains, the plain
  environment values and, when the mirror is stale, `stale_since`.

### Set or Unset an Environment Variable

1. On the environment page, under **Environment Variables**, fill in **Key** and **Value**.
2. Press **Set variable**. **Unset** on a row removes it.

- REST: `POST /v1/set_environment_env`:

```json
{"repository_id": "<repository id>", "environment": "production", "key": "LOG_LEVEL", "value": "info"}
```

- REST: `POST /v1/unset_environment_env`:

```json
{"repository_id": "<repository id>", "environment": "production", "key": "LOG_LEVEL"}
```

- MCP: `set_environment_env`, `unset_environment_env`.

The key must be an environment variable name. Lake scans `KEY=value` for credentials before
recording it; a value that looks like one is refused with *use a sealed secret*. The operator
applies the change to the environment.

### Add or Remove a Domain

1. Under **Domains**, enter the **Host** and choose **TLS**: **ACME** (`acme`) or **Provided**
   (`provided`).
2. Press **Create domain**. The domain is listed with its status while the operator sets it up.
3. To remove one, press **Delete** on its row and confirm **Delete domain**: the host stops serving
   the environment.

- REST: `POST /v1/create_domain`:

```json
{"repository_id": "<repository id>", "environment": "production", "host": "reports.example.com", "tls": "acme"}
```

- REST: `POST /v1/delete_domain`:

```json
{"repository_id": "<repository id>", "environment": "production", "host": "reports.example.com"}
```

- MCP: `create_domain`, `delete_domain`.

A host that is not a DNS host name is refused at `/host`.

### Read the Logs

**View logs** opens the environment's container logs, followed every 3 seconds; **Refresh** reloads
them. Each line names its pod and container.

- REST: `GET /v1/environment_logs?repository_id=<repository id>&environment=production&limit=200&direction=backward`
- MCP: `environment_logs`. The answer is `items` (`ts`, `pod`, `container`, `line`) and `next_since`;
  pass `next_since` as `since` to read on from there. `direction` is `forward` or `backward`.

## Limits

- Deployments, rollbacks and their images come from `liquid deploy`; the pages show them and change
  variables and domains, but do not create a deployment.
- `environment_logs` answers 1 to 1,000 lines per call.
- Environment variables are plain values only; keep credentials in a secret
  ([Secrets and Encryption Keys](/docs/secrets)).
- A repository with no workspace has no environments: calls answer `conflict` at `/repository_id`.
- The pages show only the environments you may read; another person may see fewer.

## Permissions

Each environment has its own roles: `read`, `write`, `deploy`, `share` and `admin`, granted directly
or inherited from the repository and the account ([Grants and Effective Access](/docs/access)).

| Step | Needs |
|---|---|
| See an environment, its history and logs | `read` on the environment |
| Set or unset a variable | `write` on the environment |
| Create or delete a domain | `deploy` on the environment |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| *Nothing Deployed Yet* | No repository you can read has an environment. | Deploy with `liquid deploy`, or ask for `read` on the environment. |
| A stale banner, or `stale_since` | Lake cannot reach the operator's control plane; you see the last mirrored state. | Reads keep working; changes answer `store_unavailable` until the control plane is back. |
| `store_unavailable` on a change | The operator's control plane is unreachable. | Retry later. |
| `workspace_not_ready` | The repository's workspace is starting or stopped. | Start it on [Workspaces](/docs/workspaces). |
| *use a sealed secret* | The value looks like a credential. | Keep it in a secret instead. |
| `invalid_input` at `/host` | The host is not a DNS host name. | Enter a host such as `reports.example.com`, without a scheme or path. |
| `unauthorized` | You lack the role the step needs. | Ask an administrator for `write` or `deploy` on the environment. |
| An empty list where you expected environments | You may read none of the repository's environments. | Ask for `read` on them. |

## Related Pages

- [Apps](/docs/apps): Ship → Apps, the other way to run an app.
- [Packages, Builds and the Registry](/docs/registry): build the image you deploy.
- [Repositories](/docs/repositories): the repository an environment belongs to.
- [The liquid CLI](/docs/cli): `liquid deploy` and the rest of liquid.
- [Grants and Effective Access](/docs/access): roles on an environment.
