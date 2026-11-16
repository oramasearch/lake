// SPDX-License-Identifier: MIT

/** Data → MCP Connections (`/w/<id>/mcp-connections`, lake#652, docs/ui/mcp-connections.md): the
 * remote MCP servers this workspace is a client of, each with its status, its tools and its calls
 * this month. New connection asks for the URL and how to authenticate, then Lake discovers the
 * server's tools; the connection's page decides which are allowed. */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { Activity, Plug, Plus, Wrench } from "lucide-react";
import { date } from "@/lib/format";
import { Button, Card, Chip, Empty, ErrorBlock, Page, Skeleton, Stat, StatRow } from "@/lib/ui";
import { useToast } from "@/lib/toast";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useSpaceRead, useWsPath } from "@/pages/train/workspace";
import { authLabel, hostOf, statusLabel, statusTone, type McpConnection } from "./logic";
import { NewMcpConnection } from "./NewMcpConnection";

type ListAnswer = { connections?: McpConnection[] };

export default function McpConnectionsPage() {
  const navigate = useNavigate();
  const toast = useToast();
  const wsPath = useWsPath();
  const [creating, setCreating] = useState(false);
  const list = useSpaceRead<ListAnswer>("list_mcp_connections");
  const rows = list.data?.connections ?? [];
  const allowed = rows.reduce((sum, r) => sum + (r.allowed_count ?? 0), 0);
  const calls = rows.reduce((sum, r) => sum + (r.calls_this_month ?? 0), 0);
  const needing = rows.filter((r) => r.status !== "connected").length;

  const newButton = (
    <Button variant="primary" onClick={() => setCreating(true)}>
      <Plus size={15} aria-hidden /> New connection
    </Button>
  );

  return (
    <Page
      eyebrow="Data"
      title="MCP Connections"
      sub="Other MCP servers this workspace uses. Lake is their client: it holds the credential, and people, Ask Orama and agents use the tools you allow."
      actions={newButton}
    >
      <WorkspaceGate returnTo="/mcp-connections">
        <StatRow cols={3}>
          <Stat label="Connections" icon={Plug} value={list.isLoading ? "…" : rows.length} sub={needing ? `${needing} need attention` : "all connected"} />
          <Stat label="Tools Allowed" icon={Wrench} value={list.isLoading ? "…" : allowed} sub="across every connection" />
          <Stat label="Calls This Month" icon={Activity} value={list.isLoading ? "…" : calls} sub="to these servers, metered" />
        </StatRow>
        {list.isLoading ? (
          <Skeleton lines={4} />
        ) : list.isError ? (
          <ErrorBlock error={list.error} retry={() => void list.refetch()} what="MCP connections" />
        ) : !rows.length ? (
          <Empty icon={Plug} title="No MCP connections yet" action={newButton}>
            Connect a remote MCP server by its URL. Lake discovers its tools, and you choose which this workspace may use and which count as changes a person confirms.
          </Empty>
        ) : (
          <ul className="m-0 grid list-none gap-2 p-0" aria-label="MCP connections">
            {rows.map((row) => (
              <li key={row.id} className="min-w-0">
                <ConnectionCard row={row} to={wsPath(`/mcp-connection?${qs({ id: row.id })}`)} />
              </li>
            ))}
          </ul>
        )}
      </WorkspaceGate>
      {creating && (
        <NewMcpConnection
          onClose={() => setCreating(false)}
          onCreated={(c) => {
            setCreating(false);
            toast(c.status === "connected" ? `Connected to ${c.name}` : `${c.name} added`);
            navigate(wsPath(`/mcp-connection?${qs({ id: c.id })}`));
          }}
        />
      )}
    </Page>
  );
}

function ConnectionCard({ row, to }: { row: McpConnection; to: string }) {
  return (
    <Card to={to} padded={false}>
      <div className="px-4 py-3">
        <div className="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <span className="min-w-0 flex-1 truncate text-[14.5px] font-semibold text-ink">{row.name}</span>
          <Chip tone={statusTone(row.status)}>{statusLabel(row.status)}</Chip>
        </div>
        <div className="mt-1 truncate font-mono text-[12px] text-ink-3">{row.kind === "stdio" && row.stdio ? row.stdio.package : hostOf(row.url)}</div>
        <div className="mt-2 flex flex-wrap items-center gap-1.5">
          <Chip>
            {row.allowed_count} of {row.tool_count} tools allowed
          </Chip>
          <Chip>{authLabel(row.auth?.kind)}</Chip>
          {row.exported && <Chip tone="action">Through Lake's MCP</Chip>}
          {row.agent_id && <Chip tone="ai">Has an agent</Chip>}
          <span className="text-[12px] text-ink-3">
            {row.calls_this_month ?? 0} calls this month{row.refreshed_at ? ` · refreshed ${date(row.refreshed_at)}` : ""}
          </span>
        </div>
        {row.last_error && row.status !== "connected" && <p className="mt-2 mb-0 text-[12.5px] text-bad">{row.last_error}</p>}
      </div>
    </Card>
  );
}
