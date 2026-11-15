// SPDX-License-Identifier: MIT

/** Network → Activity (`/w/<id>/network/activity`, lake#729, docs/ui/network.md): the workspace's
 * recent network decisions, what reached what and whether it was allowed (`network_activity`).
 * The records are the workspace's own; until the cluster reports them, the page says so rather
 * than showing an empty list. Changes to the rules are in Organization → Activity. */
import { useState } from "react";
import { Link } from "react-router-dom";
import { Activity } from "lucide-react";
import type { Row } from "@/lib/api";
import { dateTime } from "@/lib/format";
import { DataTable, Empty, Toolbar } from "@/lib/ui";
import { recordColumns } from "./network";
import { NetworkPage, NotRecorded, useNetworkRead } from "./parts";

const LIMITS = [50, 100] as const;

export default function ActivityPage() {
  const [limit, setLimit] = useState<number>(50);
  const q = useNetworkRead<Row>("network_activity", { limit });
  const events = Array.isArray(q.data?.events) ? (q.data!.events as Row[]) : [];
  return (
    <NetworkPage
      title="Network Activity"
      sub={
        <>
          What reached what in this workspace, and whether it was allowed. Who changed a rule, and when, is in <Link to="/activity">Organization → Activity</Link>.
        </>
      }
    >
      {q.data && q.data.available !== true ? (
        <NotRecorded what="Network Activity" page={q.data} />
      ) : (
        <>
          <Toolbar>
            <label className="inline-flex items-center gap-2 text-[12.5px]">
              Show the last
              <select value={limit} onChange={(e) => setLimit(Number(e.target.value))} aria-label="How many">
                {LIMITS.map((n) => (
                  <option key={n} value={n}>
                    {n}
                  </option>
                ))}
              </select>
            </label>
          </Toolbar>
          <DataTable<Row>
            q={q}
            what="network activity"
            caption="Network activity"
            rows={q.data ? events : undefined}
            empty={
              <Empty icon={Activity} title="No Network Activity Yet">
                Nothing in this workspace has reached or been refused anything yet.
              </Empty>
            }
            columns={[
              { key: "when", label: "When", render: (r) => <span className="font-mono text-[12px]">{dateTime(recordColumns(r).when)}</span> },
              { key: "from", label: "From", render: (r) => recordColumns(r).source || "—" },
              { key: "to", label: "To", render: (r) => recordColumns(r).destination || "—" },
              { key: "outcome", label: "Outcome", render: (r) => recordColumns(r).outcome || "—" },
            ]}
          />
        </>
      )}
    </NetworkPage>
  );
}
