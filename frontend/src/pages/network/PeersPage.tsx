// SPDX-License-Identifier: MIT

/** Network → Private Networks (`/w/<id>/network/private-networks`, lake#729, docs/ui/network.md): the
 * peer connections joining another network (a cluster, another Lake's workspace, a cloud network) to
 * this workspace, what each side offers the other, and whether it is up (`list_network_peers`).
 * Read-only: a peer is made through the operator's exchange with the other side, not here. */
import { useState } from "react";
import { Network } from "lucide-react";
import type { Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { DataTable, Empty, Mono, Toolbar } from "@/lib/ui";
import { nameOf, present } from "./network";
import { NetworkPage, RecordState, useNames, useNetworkRead } from "./parts";

export default function PeersPage() {
  const [history, setHistory] = useState(false);
  const q = useNetworkRead<{ peers?: Row[] }>("list_network_peers");
  const { names } = useNames();
  const all = q.data?.peers ?? [];
  const withdrawn = all.length - present(all, false).length;
  return (
    <NetworkPage
      title="Private Networks"
      sub="Other networks joined to this workspace: what this workspace offers them, and what it reaches of theirs. Each side approves a peer connection, so it is set up with your Lake operator rather than here."
    >
      {withdrawn > 0 && (
        <Toolbar>
          <label className="inline-flex items-center gap-2 text-[12.5px]">
            <input type="checkbox" checked={history} onChange={(e) => setHistory(e.target.checked)} /> Show {withdrawn} removed
          </label>
        </Toolbar>
      )}
      <DataTable<Row>
        q={q}
        what="private networks"
        caption="Peer connections"
        rows={q.data ? present(all, history) : undefined}
        rowKey={(p) => String(p.id)}
        initialSort={{ key: "name", dir: "ascending" }}
        empty={
          <Empty icon={Network} title="No Private Networks">
            This workspace is joined to no other network. A peer connection appears here once your Lake operator sets it up with the other side.
          </Empty>
        }
        columns={[
          { key: "name", label: "Name", sort: (p) => String(p.name), render: (p) => <span className="font-semibold">{String(p.name)}</span> },
          { key: "remote", label: "Other side", render: (p) => <Mono short={40}>{String(p.remote ?? "")}</Mono> },
          {
            key: "exports",
            label: "Offers",
            render: (p) => {
              const list = Array.isArray(p.exports) ? (p.exports as Row[]) : [];
              return list.length ? list.map((e) => `${nameOf(names, String(e.target_kind), String(e.target_id))}:${String(e.port)}`).join(", ") : <span className="muted">Nothing</span>;
            },
          },
          {
            key: "imports",
            label: "Reaches",
            render: (p) => {
              const list = Array.isArray(p.imports) ? (p.imports as Row[]) : [];
              return list.length ? <span className="font-mono text-[12px]">{list.map((i) => `${String(i.name)}:${String(i.port)}`).join(", ")}</span> : <span className="muted">Nothing</span>;
            },
          },
          { key: "status", label: "Status", render: (p) => <RecordState record={p} /> },
          { key: "updated", label: "Updated", sort: (p) => String(p.updated_at), render: (p) => <span className="font-mono text-[12px] text-ink-2">{relative(p.updated_at)}</span> },
        ]}
      />
    </NetworkPage>
  );
}
