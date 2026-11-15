# Secure networking: architecture

How a workspace becomes a locked-down sandbox with only declared paths, end to end: what a person
does in Lake, what Lake records and where, what the operator enforces, and what carries the bytes.
Epic: [lake#724](https://github.com/oramasearch/lake/issues/724) (Lake's half),
[orama-operator#437](https://github.com/oramasearch/orama-operator/issues/437) (the operator's).

Related pages:
- **The design and its decisions (D1–D8)**: the operator's
  [`docs/infrastructure/secure-networking.md`](https://github.com/oramasearch/orama-operator/blob/dev/docs/infrastructure/secure-networking.md).
  Its section 11 is the Lake↔operator contract.
- **The data path**: zetta-tunnel's
  [`docs/architecture.md`](https://github.com/oramasearch/zetta-tunnel/blob/dev/docs/architecture.md).
- **Lake's own contracts**:
  - [`control-plane/network.md`](../control-plane/network.md) (the tables);
  - [`api/network.md`](../api/network.md) (the capabilities);
  - [`api/tunnel-grants.md`](../api/tunnel-grants.md) (tunnel credentials);
  - [`api/scoped-credentials.md`](../api/scoped-credentials.md) (credentials on a connection).

## What it gives a customer

Workspaces are locked-down sandboxes, and every network and DNS connection is explicit.

| A customer can | Lake capability | Enforced by |
|---|---|---|
| See what a workspace can reach and what can reach it. The default is nothing. | `network_rules` | operator `NetworkSandbox` (Cilium) |
| Let one app use one service, on named ports | `create_network_connection`, `remove_network_connection` | operator `Connection` (paired Cilium policy) |
| Give that app a credential scoped to what it needs, and rotate or revoke it | `create_network_connection` with a scope, `rotate_network_connection_credential` | operator native role (Postgres first); workspace Zetta app credential (zetta#1280) |
| Let a workload reach the outside, by address or by DNS name | `create_network_egress_rule` | operator `EgressRule`: Cilium for CIDR, zetta-tunnel egress gateway for names |
| Let a developer reach one target from their machine, for a time | `grant_network_tunnel`, `issue_network_tunnel_certificate`, `revoke_network_tunnel` | operator `Tunnel` → zetta-tunnel relay and endpoint |
| Join another cluster or private network for named services | `list_network_peers` | operator `PeerConnection` → zetta-tunnel peer gateways |
| See who connected and what was refused | `list_network_tunnel_sessions`, `network_activity` | records in the workspace Zetta (zetta#1281) |

Every action is a capability first: REST, OpenAPI and MCP from one registry. The **Network**
section of each workspace (lake#729) is a page over the same capabilities. Lake names a tunnel,
never its vendor. Data → Connections (datasets, lake#223) is a different thing from an app's
connection, and is labelled so.

## The three layers

```text
 Person ── UI · REST · MCP ──▶ Lake
                                │
        ┌───────────────────────┴────────────────────────┐
        ▼ desired state (D1)                              ▼ records people see (D1)
 Lake's Postgres control plane                    workspace Zetta (one per workspace)
   0084_network: network_connections,               grants history, tunnel sessions,
   network_egress_rules, network_tunnel_grants,     network activity (zetta#1281)
   network_peers  + *_status (operator-owned)
   0085: tunnel client/relay CAs
   NOTIFY lake_network
        │
        ▼ reads Postgres only
 orama-operator (identical on every cluster)
   NetworkSandbox · Connection · EgressRule · Tunnel · PeerConnection
        │                                   │
        ▼                                   ▼
 Cilium policy + native datastore roles    zetta-tunnel roles, from projected files:
 (credentials as workspace-secret           relay · endpoint · egress · peer · ingress · mesh
  projections)                              (mTLS 1.3 over Corosio, shared with Zetta)
```

### Lake: what is declared

- **Desired state in Postgres** (decision D1). It is the multitenancy-and-dispatch exception: the
  operator never reads a workspace Zetta, so what it must enforce lives where it
  reads.
  - Lake writes four desired tables. The operator writes only their `*_status` twins.
  - `network_guard` tells the operator the schema is present; it reads nothing until it exists.
  - `NOTIFY lake_network` wakes it on a change.
- **Records in the workspace Zetta**:
  - grants as a person sees them, tunnel sessions, and the network activity of every allow and
    deny;
  - the relay, endpoints and gateways write those records, and Lake reads them for the pages.
- **Rollout without breakage.** `0084_network` backfills one connection from every existing app
  to every service in its workspace, so the operator's retirement of `allow-workspace-internal`
  changes nothing until a person withdraws a path ([`control-plane/network.md`](../control-plane/network.md)).
- **Tunnel trust.**
  - Lake signs the developer's CSR with the customer's PKI root, pinned on the grant.
  - The certificate names the grant and lives at most 12 hours.
  - The operator returns the relay's endpoint and CA in status.
  - Lake signs only when both directions of trust are ready ([`api/tunnel-grants.md`](../api/tunnel-grants.md)).
- **Credentials.**
  - A connection may carry a scope. Lake shows the workspace-secret reference, never the value.
  - Rotation advances a generation; the operator replaces the credential.
  - Removing the connection revokes it ([`api/scoped-credentials.md`](../api/scoped-credentials.md)).

### The operator: what is enforced

The same operator, with the same kinds, runs on every cluster.

| Kind | From | Produces |
|---|---|---|
| `NetworkSandbox` | every Lake instance, workspace and Orama Cloud v1 stack | Default-deny in and out. Explicit allowances for the platform (operators, API server, DNS, Lake). A Cilium deny for cloud metadata and host-service addresses. |
| `Connection` | `network_connections` | Paired policies, one source to one target on named ports. A scoped native role delivered as a projection of a workspace secret (orama-operator#469). |
| `EgressRule` | `network_egress_rules` | CIDR: Cilium policy. DNS name: the workspace's egress gateway, with Cilium allowing only the gateway (orama-operator#470). A workspace's `internetEgress` keeps its effect. |
| `Tunnel` | `network_tunnel_grants` | One relay per cluster (LoadBalancer, 443), one endpoint per grant, the grants file and CA bundle (orama-operator#471). A customer's own Cloudflare account can be another provider. |
| `PeerConnection` | `network_peers` | A peer gateway at each end, exporting and importing named services (orama-operator#472). |

Tenants cannot write network policy or these kinds; only the operator can (orama-operator#467).
Ingress through zetta-tunnel (orama-operator#473) and the mesh node agent (orama-operator#474) are
platform layer on every cluster, installed the same way on the management cluster and on leaves.

### zetta-tunnel: what carries the bytes

Orama's own C++ program (decisions D3, D5–D8): one binary, one private image, and a role per
process. It never calls Lake or the Kubernetes API; it acts only on the files the operator projects.
Every hop is mutual TLS 1.3. Its architecture is in
[zetta-tunnel's `docs/architecture.md`](https://github.com/oramasearch/zetta-tunnel/blob/dev/docs/architecture.md).

## Flows

**An app uses a service with a read-only credential.**
1. A person declares the connection with a scope (`create_network_connection`).
2. Lake writes the row and notifies.
3. The operator:
   - materializes a `Connection`;
   - opens the path in both policies;
   - creates a Postgres role with that scope;
   - writes the credential into the app's workspace secret;
   - sets the status.
4. Lake shows the connection Ready, with the secret reference.
5. Removing the connection closes the path and revokes the role.

**A developer opens a tunnel.**
1. A person grants a member access to one target until a time (`grant_network_tunnel`).
2. The operator projects the grant to the relay and starts an endpoint for it, then reports the
   relay's address and CA.
3. The member's client sends a CSR. Lake signs it (`issue_network_tunnel_certificate`).
4. The client connects through the relay to the endpoint, then to the one target.
5. Revocation or expiry removes the grant from the projection, and its sessions close.
6. Lake shows the session history from the workspace Zetta.

**A workload calls an outside API by name.**
1. A person declares egress to `api.example.com:443` (`create_network_egress_rule`).
2. The operator adds the name to the workspace's egress gateway and allows the workload only the
   gateway.
3. The gateway authorizes the name and port before resolving it, and forwards the TLS unchanged.

## Where things stand

The live and in-review state is tracked in [`design/status.md`](../design/status.md):
- `NetworkSandbox`, tenant RBAC and `Connection` are live on every cluster.
- Lake's tables, capabilities, tunnel grants and scoped credentials are in review (#732–#735).
- The remaining operator kinds and the Network section are in progress.

**Before the release**, origin TLS on every Lake Ingress is required (lake#726, orama-operator#457).
