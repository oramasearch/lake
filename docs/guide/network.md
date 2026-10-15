# Network

Each workspace is a closed network: its apps, services and agents reach only what a rule allows,
and nothing outside reaches in unless a rule says so. **Network** in the rail, after Ship, is where
you see and change those rules for the workspace you are in: which app may reach which service and
with what credential, which developer may open a tunnel to it from their own machine, which outside
hosts the workspace may reach, and which other networks are joined to it.

Lake records each rule and asks the cluster to apply it; every rule shows whether the cluster has.
A rule waits (**Waiting**) until the cluster reports on it, so a rule that shows **Ready** is one
the cluster applied.

Every REST and MCP call on this page names your workspace: `customer_id` is the id of the account
that owns your workspaces and `workspace_id` is the workspace. Each call is also an MCP tool of the
same name.

## The Pages

| Page | What it shows |
|---|---|
| **Overview** | How many connections, developer grants, private networks and outside destinations the workspace has, how many rules the cluster has applied, and recent tunnel sessions and activity |
| **Connections** | Which app or service may reach which, on which ports, with what credential |
| **Developer Access** | Who may open a tunnel to a service or app, until when; their sessions |
| **Private Networks** | Other networks joined to the workspace, and what each side offers |
| **Egress and DNS** | The outside hosts and address ranges the workspace may reach |
| **Policies** | Every rule in effect, written as a sentence, and whether it is applied |
| **Activity** | What reached what, and whether it was allowed |

Each rule's **Status** reads **Waiting** (the cluster has not reported), **Updating** (a change is
not applied yet), **Ready**, **Failed** (hover for the cluster's message), or **Removing** and
**Removed** after you remove it. Removed rules stay in the history: tick **Show removed** to see
them.

## Connections

A **connection** lets one app or service reach another. Without one, nothing in the workspace
reaches anything else. This is not **Data → Connections**, which lists the databases the workspace
reads for datasets ([Connections](/docs/connections)).

### Connect an App to a Service

1. Open **Network → Connections** and press **Connect an app**.
2. Choose **From** (an app or a service) and **To** (a service or an app).
3. Leave **Ports** empty for every port the target serves, or list them (`5432, 6432`).
4. From an app to a service you may give it its own **Credential**: **Read only** or **Read and
   write**, and optionally one **Schema**. The cluster makes a role on the service for this
   connection alone and puts it in a workspace secret the app mounts; Lake shows the secret's
   name, never its value.
5. Press **Connect**.

`POST /v1/create_network_connection`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "source_kind": "app", "source_id": "<app id>", "target_kind": "service", "target_id": "<service id>", "ports": [5432], "credential_scope": {"access": "read", "schema": "analytics"}}
```

List them, removed ones included, with
`GET /v1/list_network_connections?customer_id=<customer id>&workspace_id=<workspace id>`.

### Rotate a Connection's Credential

Press **Rotate credential** on the row and confirm. The cluster replaces the role's credential and
rewrites the same secret, so the app keeps its one reference. The row shows **Updating** until it
has. A connection without a credential has nothing to rotate.

`POST /v1/rotate_network_connection_credential`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "connection_id": "<connection id>"}
```

### Remove a Connection

Press **Remove** on the row and confirm. The app can no longer reach the target once the cluster
applies it, and the connection's credential is revoked.

`POST /v1/remove_network_connection`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "connection_id": "<connection id>"}
```

## Developer Access

A **tunnel grant** lets one member of the account reach one port of a service or app from their own
machine, until it expires. Nothing is exposed to the internet: the person connects as themselves,
with a certificate made for that grant and for a key only they hold.

### Grant Access

1. Open **Network → Developer Access** and press **Grant access**.
2. Choose **Who** (you, or a member of the account's organization), what it **Reaches** (a service
   or an app; for a deployment, choose the service that serves it), the **Port**, and **For** how
   long (1 hour to 30 days).
3. Press **Grant access**.

`POST /v1/grant_network_tunnel`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "user_id": "<user id>", "target_kind": "service", "target_id": "<service id>", "port": 5432, "expires_at": "2026-10-15T00:00:00Z"}
```

`expires_at` is a future time in UTC. List grants, with expired and revoked ones, with
`GET /v1/list_network_tunnel_grants?customer_id=<customer id>&workspace_id=<workspace id>`.

### Get Your Tunnel Credentials

Only the person granted can get credentials, and only once the grant's **Tunnel** column reads
**Ready**. You can also start from Ship → Services: a service's **Developer access** opens Grant
access for it, for you, for an hour.

1. On your grant's row press **Get credentials**. Every step below is a block with a Copy button;
   paste each into a terminal on your own machine, in order.
2. **Make a key and a request.** The first block makes a folder for this grant, then
   `tunnel.key` (your key, which stays on your machine) and `tunnel.csr`, and prints the request:

   ```sh
   umask 077
   openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
     -keyout tunnel.key -out tunnel.csr -subj '/CN=developer'
   ```

3. Paste what it printed and press **Get certificate**.
4. **Save the certificate.** One block writes `tunnel.crt` and `relay-ca.crt` into the same
   folder. Lake does not keep them to show again; while your access lasts you can ask for another
   certificate.
5. **Start the tunnel** with the next block, and leave it running. It needs the tunnel client,
   `zetta-tunnel`. The command already holds your endpoint, and listens only on your machine, on a
   port next to the target's (5433 for Postgres's 5432).
6. **Connect** with the last block, in a second terminal. If you administer the account, the block
   already holds the service's own user, password and database: the password is hidden until you press
   **Reveal**, and **Copy** copies the whole working command. Otherwise it reads, for Postgres,
   `psql "postgresql://USER@127.0.0.1:5433/DATABASE"`, and the user, password and database are under
   Ship → Services, **Show connection**. The tunnel does not replace the service's own sign-in.

The tunnel stops working when your access expires or is revoked, and your certificate lasts at
most 12 hours.

`POST /v1/issue_network_tunnel_certificate`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "tunnel_grant_id": "<grant id>", "csr_pem": "-----BEGIN CERTIFICATE REQUEST-----\n...\n-----END CERTIFICATE REQUEST-----\n"}
```

A certificate lasts at most twelve hours, and never beyond the grant.

### Revoke Access

Press **Revoke** on the row and confirm. The person can no longer connect, and the cluster ends any
session through the grant.

`POST /v1/revoke_network_tunnel`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "tunnel_grant_id": "<grant id>"}
```

**Recent Sessions** lists who connected, to what and when, from
`GET /v1/list_network_tunnel_sessions?customer_id=<customer id>&workspace_id=<workspace id>`.

## Private Networks

A **peer connection** joins another network (one of your clusters, another workspace, a cloud
network) to this workspace. **Network → Private Networks** lists each one: its name, the other
side, what this workspace **Offers** it, what this workspace **Reaches** of theirs, and its status.
Both sides approve a peer connection, so it is set up with your Lake operator; this page shows it.

`GET /v1/list_network_peers?customer_id=<customer id>&workspace_id=<workspace id>`

## Egress and DNS

An **egress rule** lets the workspace's workloads reach one outside destination: a host by its DNS
name (`api.example.com`, or `*.example.com`) or an address range (`203.0.113.0/24`), on the ports
it needs. A host named by DNS is resolved and checked by name. A rule is for the whole workspace or
for one app or service. **All**, **DNS names** and **Address ranges** filter the list.

1. Open **Network → Egress and DNS** and press **Allow a destination**.
2. Choose **DNS name** or **Address range** and type it, list the **Ports** (443 is filled in), and
   choose **For**: **The whole workspace**, **One app** or **One service**.
3. Press **Allow**.

`POST /v1/create_network_egress_rule`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "dns_name": "api.example.com", "ports": [443]}
```

For one app, add `"source_kind": "app"` and `"source_id"`; for an address range, send `"cidr"`
instead of `"dns_name"`. List them with
`GET /v1/list_network_egress_rules?customer_id=<customer id>&workspace_id=<workspace id>`.

## Policies

**Network → Policies** writes every rule in effect as a sentence, such as *App reporting may reach
service orders on TCP 5432, with its own read only credential.*, and ends with what is not asked for: *Lake
asks for no other path into or out of this workspace.* Below, **Each Rule** lists them with whether
the cluster has applied them; a rule's kind links to the page that changes it.

`GET /v1/network_rules?customer_id=<customer id>&workspace_id=<workspace id>`

## Activity

**Network → Activity** lists what reached what and whether it was allowed, the last 50 or 100.
Who changed a rule, and when, is in **Organization → Activity** ([Activity and Audit](/docs/activity)).

`GET /v1/network_activity?customer_id=<customer id>&workspace_id=<workspace id>&limit=50`

## Limits

- Tunnel sessions and network activity are not recorded yet. **Recent Sessions** and **Activity**
  say *Not Recorded Yet* instead of listing anything; the rules still apply.
- An egress rule cannot be removed yet, and a peer connection is not created here.
- A credential is offered only from an app to a service.

## Permissions

| To | You need on the account |
|---|---|
| See any Network page | read |
| Connect, rotate a credential, remove a connection, allow a destination | write |
| Grant or revoke developer access | admin |
| Get tunnel credentials | read, and to be the person granted |

A refused change says what was missing in the drawer or the confirmation.

## Troubleshooting

- **Get credentials is greyed out.** The grant's tunnel is not **Ready** yet: the cluster has not
  reported its endpoint. Wait, or ask your Lake operator.
- **"the caller must be the grant's recipient".** Only the person granted can get credentials,
  even an admin who made the grant.
- **A rule stays Waiting.** The cluster has not applied it. Hover the status for its message.
- **"user_id must be a member of the workspace's account".** Grant access only to a member who can
  read the account.

## Related Pages

- [Connections](/docs/connections): the databases a workspace reads for datasets.
- [Services and Storage](/docs/services): the services a connection reaches.
- [Apps](/docs/apps): the apps that connect.
- [Secrets and Encryption Keys](/docs/secrets): where a connection's credential is kept.
- [Activity and Audit](/docs/activity): who changed what.
