// SPDX-License-Identifier: MIT

/** Network → Overview (`/w/<id>/network/overview`, lake#729, docs/ui/network.md): the workspace's
 * network at a glance. Figures for what Lake asks the cluster to allow (connections, developer
 * access, private networks, egress), how many of those the cluster has applied, then the recent
 * tunnel sessions and network activity, and where each part is managed. */
import { Link } from "react-router-dom";
import { Cable, Database, Globe, KeyRound, Network, ShieldCheck } from "lucide-react";
import type { Row } from "@/lib/api";
import { Card, DataTable, Grid, Query, Stat, StatRow } from "@/lib/ui";
import { relative } from "@/lib/format";
import { useWsPath } from "@/pages/train/workspace";
import { ACCESS_PATH, ACTIVITY_PATH, CONNECTIONS_PATH, EGRESS_PATH, PEERS_PATH, POLICIES_PATH, recordColumns, recordState } from "./network";
import { NetworkPage, NotRecorded, useNetworkRead } from "./parts";

export default function OverviewPage() {
  const ws = useWsPath();
  const rules = useNetworkRead<Row>("network_rules");
  const sessions = useNetworkRead<Row>("list_network_tunnel_sessions", { limit: 10 });
  const activity = useNetworkRead<Row>("network_activity", { limit: 10 });
  const list = (key: string) => (Array.isArray(rules.data?.[key]) ? (rules.data![key] as Row[]) : []);
  const all = ["connections", "egress_rules", "tunnel_grants", "peers"].flatMap(list);
  const waiting = all.filter((r) => recordState(r).label !== "Ready").length;
  const figure = (key: string) => (rules.data ? list(key).length : "—");
  return (
    <NetworkPage
      title="Overview"
      sub="What this workspace's apps, services and developers may reach, and what may reach them. Nothing is open unless a rule here asks for it."
    >
      <Query q={rules} what="the network rules" lines={2}>
        {() => (
          <StatRow cols={4}>
            <Stat label="Connections" icon={Cable} value={figure("connections")} sub="apps and services joined" to={ws(CONNECTIONS_PATH)} />
            <Stat label="Developer access" icon={KeyRound} value={figure("tunnel_grants")} sub="active tunnel grants" to={ws(ACCESS_PATH)} />
            <Stat label="Private networks" icon={Network} value={figure("peers")} sub="peer connections" to={ws(PEERS_PATH)} />
            <Stat label="Egress" icon={Globe} value={figure("egress_rules")} sub="outside destinations allowed" to={ws(EGRESS_PATH)} />
          </StatRow>
        )}
      </Query>
      {rules.data && (
        <p className="muted mt-0 mb-5 text-[12.5px]">
          <ShieldCheck size={13} className="mr-1 inline align-[-2px]" aria-hidden />
          {all.length === 0
            ? "Lake asks for no path into or out of this workspace."
            : waiting === 0
              ? `The cluster has applied all ${all.length} rules.`
              : `${waiting} of ${all.length} rules are waiting for the cluster.`}{" "}
          <Link to={ws(POLICIES_PATH)}>Read the policy</Link>
        </p>
      )}
      <Grid className="gap-4 lg:grid-cols-2">
        <Card label="Developer access" title="Recent Tunnel Sessions" actions={<Link to={ws(ACCESS_PATH)}>Developer access</Link>}>
          <Records q={sessions} field="sessions" what="tunnel sessions" />
        </Card>
        <Card label="Activity" title="Recent Network Activity" actions={<Link to={ws(ACTIVITY_PATH)}>All activity</Link>}>
          <Records q={activity} field="events" what="network activity" />
        </Card>
      </Grid>
      <p className="muted mt-5 text-[12.5px]">
        <Database size={13} className="mr-1 inline align-[-2px]" aria-hidden />
        The databases this workspace reads for datasets are not network connections; they are under <Link to={ws("/connections")}>Data → Connections</Link>.
      </p>
    </NetworkPage>
  );
}

function Records({ q, field, what }: { q: ReturnType<typeof useNetworkRead<Row>>; field: "sessions" | "events"; what: string }) {
  const rows = Array.isArray(q.data?.[field]) ? (q.data![field] as Row[]) : [];
  if (q.data && q.data.available !== true) return <NotRecorded what={what === "tunnel sessions" ? "Tunnel Sessions" : "Network Activity"} page={q.data} />;
  return (
    <DataTable<Row>
      q={q}
      what={what}
      rows={q.data ? rows : undefined}
      empty={`No ${what} yet.`}
      columns={[
        { key: "when", label: "When", render: (r) => <span className="font-mono text-[12px]">{relative(recordColumns(r).when)}</span> },
        { key: "source", label: "From", render: (r) => recordColumns(r).source || "—" },
        { key: "destination", label: "To", render: (r) => recordColumns(r).destination || "—" },
        { key: "outcome", label: "Outcome", render: (r) => recordColumns(r).outcome || "—" },
      ]}
    />
  );
}
