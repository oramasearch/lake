# Network

This file covers the Network group: **Overview**, **Connections** (which apps and services may reach
which), **Developer Access** (tunnel grants and their credentials), **Private Networks**, **Egress and
DNS**, **Policies** and **Activity**. The pages read Lake's control plane, so they show without the
workspace being ready and use no workspace gate. A row's state comes from the cluster's report, so
some Expect lines wait for the operator.

## Before you start

- Personas: `owner` (admin on `W1`), `member` (read on `W1`, and a member of the account that may
  receive a grant), `writer` (write, not admin, on `W1`).
- Test data: `TD-SERVICE` (a Ready Postgres in `W1`), an app in `W1` (`qa-<MMDD>-app` from
  [13-ship.md](13-ship.md), or `TD-IMAGE` deployed as an app).
- This file alone also needs:
  - `openssl` and `psql` on the tester's machine (macOS on Apple silicon or Linux x86-64 for the
    tunnel client).
  - A second Ready service in `W1` (for a service-to-service connection), if no app can be made.
  - On a Lake whose operator does not yet report network state, rows stay "Waiting". Cases that need
    "Ready" are then `blocked`; ask the operator, never write the state by hand.
- Names: no Network row has a name of its own. Egress destinations use `qa-<MMDD>.example.com`.
- Network writes cannot all be undone here: an egress rule cannot be removed yet. List the run's
  egress rules in the results file for the operator to remove.

## Overview and the section

#### NET-001 · The section opens on Overview · P1
As: owner

1. Open **Network → Overview** from the rail.
2. Open `/w/<W1>/network`.

Expect:
- Address `/w/<W1>/network/overview`; title "Network", eyebrow "Network"; breadcrumb Network.
- Sub "What this workspace's apps, services and developers may reach, and what may reach them.
  Nothing is open unless a rule here asks for it."
- Tabs "Overview", "Connections", "Developer Access", "Private Networks", "Egress and DNS",
  "Policies", "Activity"; the rail lists the same items after Ship.
- Step 2 is replaced by `/network/overview`.

#### NET-002 · An empty network · P1
As: owner · Needs: `W2` with no network rule

1. Open **Network → Overview** on `W2`.

Expect:
- Figures "Connections" ("apps and services joined"), "Developer access" ("active tunnel grants"),
  "Private networks" ("peer connections"), "Egress" ("outside destinations allowed"), all `0`. Each
  figure opens its page.
- The line "Lake asks for no path into or out of this workspace." with **Read the policy**.
- Cards "Recent Tunnel Sessions" and "Recent Network Activity" each show "Tunnel Sessions Are Not
  Recorded Yet" / "Network Activity Is Not Recorded Yet" with "Lake will keep … in this workspace once
  the cluster reports them. Until then nothing is listed here, and the rules on the other pages still
  apply." (or, where Lake records them, a table or "No tunnel sessions yet.").
- The footer "The databases this workspace reads for datasets are not network connections; they are
  under Data → Connections." links to Data → Connections.

#### NET-003 · The keyboard shortcut and the tabs · P2
As: owner

1. On any page, press `g` then `n`.
2. Press each tab in turn.

Expect:
- Step 1 opens Network → Overview.
- Each tab marks only its own rail item current; Data → Connections stays in the Data group.

## Connections

#### NET-004 · No connections yet · P2
As: owner · Needs: `W2`

1. Open **Network → Connections** on `W2`.

Expect:
- Sub "Which of this workspace's apps and services may reach which, on which ports, and with what
  credential. The databases the workspace reads for datasets are under Data → Connections." (a link).
- "No Connections Yet" with "Nothing in this workspace reaches anything else until a connection says
  so. Connect an app to the service it uses." and **Connect an app**.

#### NET-005 · Connect an app to a service with a credential · P0
As: owner · Needs: an app in `W1`, TD-SERVICE

1. On `W1`, press **Connect an app**.
2. Keep **From** "App" and choose the app; keep **To** "Service" and choose TD-SERVICE.
3. Type `5432` in **Ports**, choose **Credential** "Read only", type `public` in **Schema**.
4. Press **Connect**.

Expect:
- Drawer "Connect an App", subtitle "Declare one path. The cluster opens exactly this and nothing
  else."; **Ports** help "Separated by commas. Leave empty for every port it serves."; **Credential**
  help "Its own role on the service, made for this connection and revoked with it. Lake never shows its
  value."; **Schema** shows only after a credential is chosen.
- Toast "Connected <app> to qa-<MMDD>-pg"; the row shows From (`app` and the app's name, a link to
  its page), To (`service` and the name, a link to Ship → Services searched by it), Ports `5432`,
  Credential "Read only, schema public", Status "Waiting", Created.
- Within seconds, without a reload, Status turns "Ready" and the Credential cell adds "Secret
  <name>". The secret's value is never shown.

#### NET-006 · Connect form rules · P1
As: owner

1. **Connect an app**, press **Connect** with nothing chosen.
2. Choose a **From**, press **Connect**.
3. Set **From** "Service" and **To** "Service", choose the same service on both sides, press
   **Connect**.
4. Choose two services, type `0, 70000` in **Ports**, press **Connect**.
5. Set **From** "Service": check the **Credential** field.

Expect:
- Step 1: "Choose what connects." Step 2: "Choose what it connects to." Step 3: "A connection joins
  two different things." Step 4: "Ports are whole numbers from 1 to 65535, separated by commas."
- Step 5: no **Credential** field (a credential is offered only from an app to a service).
- A kind with nothing in it reads "This workspace has no apps yet." or "This workspace has no services
  yet."

#### NET-007 · A connection with every port · P2
As: owner · Needs: two services in `W1`

1. Connect one service to the other with **Ports** empty.

Expect:
- Ports reads "Every port it serves"; Credential reads "None".

#### NET-008 · Rotate a connection's credential · P1
As: owner · Needs: the connection from NET-005, Ready

1. Press **Rotate credential**, read the dialog, and press **Rotate**.

Expect:
- Dialog "Rotate the credential?" with "The cluster replaces the credential <app> uses for
  qa-<MMDD>-pg and rewrites the same secret. The old credential stops working once it has."
- Toast "Rotating the credential for <app>"; Status reads "Updating" (hover: "The cluster last
  applied version <n> of <n+1>."), then "Ready" without a reload.
- A connection without a credential has no **Rotate credential**.

#### NET-009 · Remove a connection and see the history · P1
As: owner · Needs: the connection from NET-007

1. Press **Remove**, read the dialog, and press **Remove**.
2. Tick "Show 1 removed".

Expect:
- Dialog "Remove the connection from <from> to <to>?" with "<from> can no longer reach <to> once the
  cluster applies it. The connection stays in the history." (", and its credential is revoked" for a
  scoped one).
- Toast "Removed the connection to <to>"; the row leaves the table; the checkbox "Show 1 removed"
  appears.
- Step 2: the row is back with Status "Removing", then "Removed", and no actions.

## Developer Access

#### NET-010 · No developer access · P2
As: owner · Needs: `W2`

1. Open **Network → Developer Access** on `W2`.

Expect:
- Sub "Who may open a tunnel from their own machine to one service or app in this workspace, and until
  when. Nothing is exposed to the internet: each person connects as themselves, with a certificate
  only they can use."
- "No Developer Access" with "Grant a member of this account a tunnel to one port of a service or app,
  for as long as they need it." and **Grant access**.
- Section "Recent Sessions" ("Who connected through these grants, to what and when.") with the
  not-recorded notice or "No tunnel sessions yet."

#### NET-011 · Grant yourself access to a service · P0
As: owner · Needs: TD-SERVICE

1. Press **Grant access**.
2. Keep **Who** "You", choose TD-SERVICE in **Reaches**, type `5432` in **Port**, keep **For** "8
   hours".
3. Press **Grant access**.

Expect:
- Drawer "Grant Developer Access", subtitle "One person, one port, for a set time. They connect from
  their own machine as themselves."; **Who** help "A member of this account."; **Port** help "To reach
  a deployment, choose the service that serves it."; **For** offers "1 hour", "8 hours", "1 day", "7
  days", "30 days".
- Toast "Granted access to qa-<MMDD>-pg"; the row: Who "You", Reaches (`service` and the name), Port
  `5432`, Until (8 hours from now), Access "Active", Tunnel "Waiting" then "Ready" without a reload.
- **Get credentials** is disabled with the tooltip "The tunnel is not ready yet: the cluster has not
  reported its endpoint." until the tunnel is Ready.

#### NET-012 · Grant from Ship → Services · P1
As: owner · Needs: TD-SERVICE

1. On **Ship → Services**, press **Developer access** on TD-SERVICE.
2. Press **Cancel**.

Expect:
- Developer Access opens with **Grant Developer Access** open: **Who** "You", **Reaches** "Service"
  TD-SERVICE, **Port** the service address's port, **For** "1 hour". The address has
  `?grant=<id>&port=<port>`.
- Step 2: the query is cleared from the address; reloading does not reopen the drawer.

#### NET-013 · Grant form rules · P1
As: owner

1. **Grant access**, leave **Reaches** on "Choose…" and press **Grant access**.
2. Choose a target, type a single space in **Port** and press **Grant access**.
3. Type `5432, 6432`, then `99999`.
4. Choose **Who** = `member` and grant; choose a person who is not in the account (if listed) and
   grant.

Expect:
- Step 1: "Choose what they reach."
- Step 2: "Name at least one port." (an empty **Port** is stopped by the browser first).
- Step 3: "A grant reaches one port.", then "Ports are whole numbers from 1 to 65535, separated by
  commas."
- Step 4: the grant to `member` works; Lake refuses an outsider with "user_id must be a member of the
  workspace's account".
- With no members found, **Who** reads "No members found".

#### NET-014 · Get tunnel credentials and connect · P0
As: owner · Needs: the Ready grant from NET-011

1. Press **Get credentials**.
2. Copy step 1 into a terminal and run it.
3. Paste what it printed into step 2 and press **Get certificate**.
4. Run steps 3, 4 (if shown), 5 and 6 in order: save the files, install the client, start the tunnel,
   connect.

Expect:
- Drawer "Get Tunnel Credentials", subtitle "A certificate for your access to qa-<MMDD>-pg on port
  5432."; step 1 "1. In a terminal on your machine, make a key and a request" holds `mkdir -p
  ~/orama-tunnel/<8 chars> && cd …`, the `openssl req` line and `cat tunnel.csr`.
- After **Get certificate**: notice "Certificate Issued" with "It is valid until <time>. Lake does not
  keep a copy to show again; ask for another while your access lasts."
- "3. Save the certificate files (paste this in the same terminal)" writes `tunnel.crt` and
  `relay-ca.crt`.
- When Lake offers a client: "Your machine" chooses this machine ("macOS (Apple silicon)" or "Linux
  (x86-64)"), "4. Install the tunnel client" downloads to `~/.local/bin/zetta-tunnel` and checks its
  SHA-256; the note says the link works for 15 minutes. Without a client, step 4 is missing and the
  later steps keep their numbers 4 and 5.
- "Start the tunnel, and leave it running" listens on `127.0.0.1:5433`.
- "Connect to qa-<MMDD>-pg, in a second terminal" shows `psql "postgresql://<user>:••••••••@127.0.0.1:5433/<db>"`
  with **Reveal**; **Copy** copies the working command. `psql` connects through the tunnel.
- Disclosure "The files and the endpoint on their own" shows "Tunnel endpoint", "tunnel.crt
  (certificate and chain)", "relay-ca.crt (the tunnel's own certificate authority)". **Done** closes;
  reopening shows step 1 again (nothing kept). `tunnel.key` never leaves the machine (Network panel:
  only the request is sent).

#### NET-015 · Certificate request checks · P1
As: owner · Needs: a Ready grant of yours

1. **Get credentials**, press **Get certificate** with the box empty.
2. Paste `hello`.
3. Paste the request together with `tunnel.key`'s contents.
4. Paste a text over 16 KiB.

Expect:
- "Paste the contents of tunnel.csr.", then "That is not a certificate request: it starts with
  -----BEGIN CERTIFICATE REQUEST-----.", then "That holds a private key. Paste only tunnel.csr; the key
  stays on your machine.", then "The request is longer than 16 KiB."
- No `issue_network_tunnel_certificate` request is sent for any of these.

#### NET-016 · Connect commands by kind · P2
As: owner · Needs: grants to an app, a TiDB, Valkey or Cassandra service, and a Zetta service (where
available)

1. Get credentials for each and read the last step.

Expect:
- App: `curl http://127.0.0.1:<port>/` with "Or open http://127.0.0.1:<port>/ in your browser."
- TiDB `mysql -h 127.0.0.1 -P <port> …`, Valkey `valkey-cli …`, Cassandra `cqlsh …`, Zetta `curl
  http://127.0.0.1:<port>/v1/server_info` with "The workspace's API answers on that address; call it
  with a key for this workspace."
- A target port below 1024 listens on 10000 plus the port.

#### NET-017 · Only the recipient gets credentials · P1
As: member, then owner · Needs: a Ready grant to `member` (NET-013)

1. As `owner`, look at the `member` grant's row.
2. As `member`, open Developer Access and press **Get credentials** on it, then complete the steps.

Expect:
- Step 1: `owner` sees **Revoke** but no **Get credentials** on someone else's grant.
- Step 2: `member` sees the grant and can get a certificate. The last step keeps `USER` and `DATABASE`
  placeholders (only an account admin reads the service's sign-in) with "Its user, password and
  database are on Ship → Services, under Show connection. The tunnel does not replace them."
- `member` sees **Revoke**, but pressing it is refused (`admin` is needed).

#### NET-018 · Revoke access · P1
As: owner · Needs: the grant from NET-011, with a running tunnel from NET-014

1. Press **Revoke**, read the dialog, and press **Revoke**.
2. Tick "Show 1 expired or revoked".

Expect:
- Dialog "Revoke this access?" with "You can no longer open a tunnel to qa-<MMDD>-pg, and the cluster
  ends any session through it. Certificates already issued for it stop working."
- Toast "Revoked access to qa-<MMDD>-pg"; the row leaves the list; the running tunnel stops working.
- Step 2: the row returns with Access "Revoked", Tunnel "—" and no actions.
- A grant past its time reads "Expired" the same way.

#### NET-019 · Granting needs admin · P1
As: writer

1. On `W1`, press **Grant access**, fill it and press **Grant access**.

Expect:
- Lake's refusal shows in the drawer; no grant is added.

## Private Networks

#### NET-020 · Private networks are read-only · P1
As: owner

1. Open **Network → Private Networks**.

Expect:
- Sub "Other networks joined to this workspace: what this workspace offers them, and what it reaches of
  theirs. Each side approves a peer connection, so it is set up with your Lake operator rather than
  here."
- No button that creates or removes anything.
- With no peer: "No Private Networks" with "This workspace is joined to no other network. A peer
  connection appears here once your Lake operator sets it up with the other side."
- With a peer (made by the operator): Name, Other side, Offers (`<name>:<port>` or "Nothing"), Reaches,
  Status, Updated; "Show <n> removed" when one was withdrawn.

## Egress and DNS

#### NET-021 · No outside destinations · P2
As: owner · Needs: `W2`

1. Open **Network → Egress and DNS** on `W2`.

Expect:
- Sub "The outside destinations this workspace's apps, services and agents may reach. A destination
  named by DNS is resolved and checked by name; nothing outside is reachable unless a rule here allows
  it."
- "No Outside Destinations" with "This workspace asks to reach nothing outside it. Allow a host such
  as api.example.com, or an address range, on the ports it needs." and **Allow a destination**; no
  tabs.

#### NET-022 · Allow a DNS name for the workspace · P0
As: owner

1. On `W1`, press **Allow a destination**.
2. Keep "DNS name", type `QA-<MMDD>.Example.com`, keep **Ports** `443`, keep **For** "The whole
   workspace".
3. Press **Allow**.

Expect:
- Drawer "Allow a Destination", subtitle "One host or address range, on the ports it needs."
- Toast "Allowed QA-<MMDD>.Example.com"; the row shows the name in lower case with chip "DNS name",
  Ports `443`, For "The whole workspace", Status "Waiting" then "Ready", Added.
- The tabs read "All 1", "DNS names 1", "Address ranges 0".

#### NET-023 · Allow an address range for one service · P1
As: owner · Needs: TD-SERVICE

1. **Allow a destination**, choose "Address range", type `203.0.113.0/24`, **Ports** `443, 8443`, **For**
   "One service" and choose TD-SERVICE.
2. Press **Allow**, then press the tab "Address ranges", then "DNS names".

Expect:
- The row: chip "Address range", Ports `443, 8443`, For (`service` and the name).
- "Address ranges" shows only that row. A tab with no rows shows "No destination of this kind" /
  "Choose All to see every destination."

#### NET-024 · Egress form rules · P1
As: owner

1. **Allow a destination**, press **Allow** with the destination empty.
2. Type `not a host`; then switch to "Address range" and type `203.0.113.0`.
3. Clear **Ports**; type `abc`.
4. Choose **For** "One app" and press **Allow** without choosing one.

Expect:
- Step 1: "Name the host, such as api.example.com." ("Give an address range, such as 203.0.113.0/24."
  for a range).
- Step 2: "That is not a host name, such as api.example.com.", then "That is not an address range, such
  as 203.0.113.0/24."
- Step 3: the browser's required check, then "Ports are whole numbers from 1 to 65535, separated by
  commas."
- Step 4: "Choose the workload the rule is for." With no app: "This workspace has no apps yet."
- A wildcard `*.qa-<MMDD>.example.com` is accepted.
- No rule has a remove action (Lake has no capability for it yet).

## Policies

#### NET-025 · The policy in words · P1
As: owner · Needs: the rules made above

1. Open **Network → Policies**.

Expect:
- Sub "The rules in effect for this workspace, in words. Lake asks the cluster for exactly these paths
  and no others; each rule shows whether the cluster has applied it."
- Card "What This Workspace May Reach", one sentence per rule, for example "App <app> may reach service
  qa-<MMDD>-pg on TCP 5432, with its own read only credential.", "Every workload in the workspace may
  reach qa-<MMDD>.example.com on TCP 443.", "You may open a tunnel to service qa-<MMDD>-pg on TCP 5432
  until <YYYY-MM-DD HH:MM> UTC.", ending "Lake asks for no other path into or out of this workspace."
- No sentence names a vendor or the software that carries a tunnel.
- "Each Rule": Kind (a link to its page: "Connection", "Egress", "Developer access", "Private
  network"), Rule (`<from> → <to> (<ports>)`), Applied (the state chip).
- On `W2`: the one sentence "Lake asks for no path into or out of this workspace." and "No rules:
  nothing is asked for."

## Activity

#### NET-026 · Network activity · P1
As: owner

1. Open **Network → Activity**.

Expect:
- Title "Network Activity"; sub "What reached what in this workspace, and whether it was allowed. Who
  changed a rule, and when, is in Organization → Activity." with a link to `/activity`.
- Where Lake does not record it yet: "Network Activity Is Not Recorded Yet", no table and no selector.
- Where it does: "Show the last" 50 or 100, and a table When, From, To, Outcome; or "No Network
  Activity Yet" / "Nothing in this workspace has reached or been refused anything yet."
- Organization → Activity lists the run's connect, grant, revoke and allow actions.

## Permissions and phone width

#### NET-027 · A reader on Network · P1
As: member

1. On `W1`, open every Network tab.
2. Try **Connect an app**, **Remove**, **Rotate credential** and **Allow a destination**.

Expect:
- Every page and row is visible.
- Each write is refused with Lake's message in its drawer or dialog; nothing changes after a reload.

#### NET-028 · A writer may connect and allow · P2
As: writer

1. On `W1`, connect two services, then allow a destination `qa-<MMDD>-w.example.com`.

Expect:
- Both work (each needs `write`). Remove the connection afterwards as `owner`.

#### NET-029 · Network at phone width · P2
As: owner

1. At 390×844, open each tab, then **Get credentials** on a Ready grant.

Expect:
- The tabs scroll or wrap without a page scroll; tables scroll inside their boxes.
- The drawers fill the screen; the long commands wrap or scroll inside their copy blocks, and **Copy**
  stays reachable.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/network/EgressPage.tsx:155-158` | **For** offers the whole workspace, one app or one service; the page's sub and the form's type include agents, but there is no "One agent" | NET-024 |
| 2 | `frontend/src/pages/network/OverviewPage.tsx:32` | "Developer access" counts every grant in `network_rules` under "active tunnel grants"; check whether revoked or expired grants are counted | NET-002 after NET-018 |
| 3 | `frontend/src/pages/network/PoliciesPage.tsx:32` | "Each Rule" lists every tunnel grant `network_rules` returns, while the sentences skip grants that are not active (`network.ts:457`) | NET-025 after NET-018 |
| 4 | `frontend/src/pages/network/OverviewPage.tsx:21` | The Overview line counts every row that is not "Ready" as waiting, including removed ones | NET-002 after NET-009 |
