# Network

**Status: implemented 2026-10-14 in the source tree (lake#729); not deployed.** Each workspace has a
Network section in the rail, after Ship: what the workspace's apps, services and developers may
reach, and what may reach them. Its pages call the fourteen network capabilities of
[`docs/api/network.md`](../api/network.md) (lake#728, lake#730, lake#731) and nothing else that
writes. The section follows the research's navigation proposal (orama-operator
`docs/research/secure-networking/perplexity-report-2026-10-13.md`, section F, §26) and the design's
Lake part (orama-operator `docs/infrastructure/secure-networking.md`, §6).

The rows are Lake's control-plane desired state and the operator's observation of it (decision D1),
so the pages do not wait for the workspace to be prepared: they use the plain page frame, not the
workspace gate. A tunnel is named for what it reaches; no page names the software that carries it.

## Pages

Every page is a workspace page under `/w/<id>/network/` and carries the section's tabs (Overview,
Connections, Developer Access, Private Networks, Egress and DNS, Policies, Activity). `/network`
opens the overview. Breadcrumbs: Network › **page**.

| Page | Path | Reads | Writes | Layout |
|---|---|---|---|---|
| Overview | `/network/overview` | `network_rules`, `list_network_tunnel_sessions` (`limit: 10`), `network_activity` (`limit: 10`) | none | Figures, each a link to its page: Connections, Developer access (active grants), Private networks, Egress. One line: how many rules the cluster has applied, and a link to Policies. Two cards: Recent Tunnel Sessions and Recent Network Activity. A line pointing to Data → Connections for the databases the workspace reads |
| Connections | `/network/connections` | `list_network_connections`, `list_apps`, `list_services` | `create_network_connection`, `rotate_network_connection_credential`, `remove_network_connection` | The table: From and To (kind and name, linked to the app or service), Ports (none means every port the target serves), Credential (its scope in words and the workspace secret's name once issued, never its value), Status, Created, and Rotate credential (scoped only) and Remove. Removed connections are hidden behind *Show n removed*. **Connect an app** (header) opens the drawer: From (app or service), To (service or app), Ports, and, only from an app to a service, Credential (none, read only, read and write) and Schema |
| Developer Access | `/network/access` | `list_network_tunnel_grants`, `list_network_tunnel_sessions` (`limit: 50`), `get_customer`, `list_memberships` | `grant_network_tunnel`, `revoke_network_tunnel`, `issue_network_tunnel_certificate` | The grants: Who (You, or the member's identity), Reaches, Port, Until, Access (Active, Expired, Revoked), Tunnel (the cluster's state, while active), and Get credentials (the recipient only) and Revoke. Expired and revoked grants are behind *Show n expired or revoked*. Recent Sessions below. **Grant access** (header): Who (you and the members of the account's organization), Reaches (service or app), Port, For (1 hour to 30 days) |
| Private Networks | `/network/private-networks` | `list_network_peers` | none | Name, Other side, Offers (this workspace's exports), Reaches (the other side's imports), Status, Updated. Read-only: a peer is made through the operator's exchange with the other side |
| Egress and DNS | `/network/egress` | `list_network_egress_rules` | `create_network_egress_rule` | All, DNS names and Address ranges, with counts. The table: Destination (with its kind), Ports, For (the whole workspace or one workload), Status, Added. **Allow a destination** (header): a DNS name or address range, Ports (443 filled in), and the whole workspace or one app or service |
| Policies | `/network/policies` | `network_rules` | none | *What This Workspace May Reach*: the rules in effect as sentences ("App reporting may reach service orders on TCP 5432, with its own read only credential."), ending with "Lake asks for no other path into or out of this workspace." Then Each Rule: kind (linked to its page), the rule and whether the cluster applied it |
| Activity | `/network/activity` | `network_activity` (`limit` 50 or 100) | none | When, From, To, Outcome. Who changed a rule is in Organization → Activity |

### A row's state

A desired row's Status comes from its `status` and `generation` (`recordState` in
`network/network.ts`), the word in the badge and the cluster's message as its title:

| Row | Shown |
|---|---|
| `status` null | Waiting (Removing when `desired: absent`) |
| `observed_generation` below `generation` | Updating (Removing when absent): a rotation or change not yet applied |
| applied, `phase` Ready | Ready |
| applied, `phase` Failed (or Error, Denied, Rejected) | Failed |
| applied and `desired: absent` | Removed |

A grant's Access is its own (`revoked_at`, `expires_at`, `active`), never the cluster's phase.
**Get credentials** is enabled only when the grant is active and the cluster reports it Ready at the
grant's generation with an endpoint and relay trust (`grantReady`), which is what
`issue_network_tunnel_certificate` requires.

### Get credentials

The drawer shows the `openssl` command that makes `tunnel.key` and `tunnel.csr` on the developer's
machine, then takes the pasted request. The page refuses an empty paste, one over 16 KiB, anything
that is not a PEM certificate request, and a paste that contains a private key, before sending it.
The answer is shown once (endpoint, `tunnel.crt` as the certificate followed by its chain, and
`relay-ca.crt`) with Copy buttons, and is not kept: closing the drawer forgets it. The key never
reaches Lake.

### Sessions and activity not recorded yet

Until the workspace-record provider is available (zetta#1281), `list_network_tunnel_sessions` and
`network_activity` answer `available: false`. The pages then show *Tunnel Sessions Are Not Recorded
Yet* or *Network Activity Is Not Recorded Yet* instead of an empty table, so the missing history
never reads as "nothing happened". A failed read shows its error, as everywhere.

## Data → Connections Is Different

Data → Connections ([connections.md](connections.md), lake#651) lists the databases a workspace
reads for datasets, kept by the workspace engine. Network → Connections lists which apps may reach
which services, kept by Lake's control plane for the cluster to enforce. Both rail items are named
Connections, in different groups; each page's description says what it is and links the other.

## Navigation and Search

`NETWORK_NAV` in `routes/nav.ts` lists the rail items, and `NETWORK_PAGES` in
`network/network.ts` lists the pages with the capabilities each calls, in the same order (the test
checks both). The section's key is `network` (`SectionKey`), its icon lucide's Network, and `g n`
jumps to it. Overview is `/network/overview`, not `/network`, so it does not own the section's other
pages in the rail. Go to (⌘K) lists every page by its rail label.

## Verify

- `frontend/tests/network.test.ts`: every network capability is called from a page; the rail's
  order and labels; each page marks only its own rail item current and Data → Connections stays
  Data's; row and grant states; ports, scopes and the three forms' inputs; the request check; names
  instead of ids; the policy sentences, with no vendor named.
- `npm run build` (the tests, `tsc`, Vite and both source checks).
- In a browser at 1316 px, against a local Lake (this branch's binary, a fresh PostgreSQL control
  database with `db/schema.sql`, a password account). Walked 2026-10-14: every page; Connect an app
  (service to service) and Remove with *Show removed* reading Removing; Grant access, then Get
  credentials with a real `openssl` request and the certificate, chain, relay CA and endpoint
  returned, then Revoke; Allow a destination by DNS name; Policies' sentences; Private Networks with
  a peer row; Activity and Recent Sessions saying *Not Recorded Yet*; the Network guide under Docs;
  Data → Connections' link. The operator's observations (Ready, Failed, a relay endpoint) and the
  peer row were written into that local database by hand, standing in for the operator. Not walked:
  an app as the source (no app can be created without a prepared workspace locally), so Rotate
  credential and the Credential column were exercised only by the tests.

## Change It

- A new network capability: its page's `capabilities` in `NETWORK_PAGES`, the call on the page,
  this table, and the capability list in `tests/network.test.ts`.
- A peer is created or removed here only once Lake has a capability for it; the page is read-only
  until then. So is removing an egress rule: there is no capability for it yet.
