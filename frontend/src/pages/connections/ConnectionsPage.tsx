// SPDX-License-Identifier: MIT

/** Data → Connections (`/w/<id>/connections`, lake#651, docs/ui/connections.md): the databases the
 * workspace reads. Figures (connections, kinds, tables reachable), the list with each connection's
 * kind, host, the service it uses, status and created date, Add connection in a drawer, Connect One
 * of Your Services, and delete behind a typed confirmation. A connection opens its own page. */
import { useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { Cable, Layers, Plus, Table2 } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, Card, Chip, DataTable, Empty, ErrorBlock, Mono, Query, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, text, timeAgo, useAsync, useSpaceRead, useWorkspace, useWorkspaceReady, useWq, useWsPath, wa, wq } from "@/pages/train/workspace";
import { AddConnection } from "./forms";
import { SafeModeControl } from "./SafeMode";
import { safeModeWarning } from "@/lib/safeMode";
import { connectionHost, connectionKind, connectionPath, connectionStatus, kindLabel, serviceFor, tablesFigure } from "./connections";

export default function ConnectionsPage() {
  const ws = useWsPath();
  const ready = useWorkspaceReady();
  const [adding, setAdding] = useState(false);
  useTrail([{ label: "Data", to: "/repositories" }, { label: "Connections", to: ws("/connections") }]);
  const add = (
    <Button variant="primary" onClick={() => setAdding(true)}>
      <Plus size={14} aria-hidden /> Add connection
    </Button>
  );
  return (
    <>
      <WorkspaceFrame eyebrow="Data" title="Connections" sub={
          <>
            The databases this workspace reads. Browse their tables and load one into a dataset. Which apps may reach which services is under{" "}
            <Link to={ws("/network/connections")}>Network → Connections</Link>.
          </>
        } actions={ready ? add : null}>
        {() => <ConnectionsContent onAdd={() => setAdding(true)} />}
      </WorkspaceFrame>
      {adding && <AddConnection onClose={() => setAdding(false)} />}
    </>
  );
}

function ConnectionsContent({ onAdd }: { onAdd: () => void }) {
  const scope = useWorkspace();
  const connections = useWq<{ connections?: Row[] }>("list_connections");
  const services = useSpaceRead<{ services?: Row[] }>("list_services", {}, { workspace: false });
  const list = useMemo(() => connections.data?.connections ?? [], [connections.data]);
  const serviceList = services.data?.services ?? [];
  const [search, setSearch] = useState("");
  // Each connection's tables, for the "Tables reachable" figure: one read-only listing apiece.
  const tables = useQueries({
    queries: list.map((c) => ({
      queryKey: ["workspace_query", scope.customerId, scope.workspaceId, "list_source_tables", { connection: text(c, "name"), max_tables: 500 }],
      queryFn: ({ signal }: { signal: AbortSignal }) => wq<{ tables?: Row[]; truncated?: boolean }>(scope, "list_source_tables", { connection: text(c, "name"), max_tables: 500 }, signal),
      staleTime: 300_000,
      retry: false,
    })),
  });
  // A connection whose safe mode asks before every query (Warn All, lake#906) is not counted, and
  // not called failed: its page lists the tables once the person confirms.
  const reachable = tablesFigure(tables.map((t) => (t.isError ? (safeModeWarning(t.error) ? "warned" : "failed") : t.data)));
  const kinds = new Set(list.map(connectionKind).filter(Boolean));
  const needle = search.trim().toLowerCase();
  const shown = list.filter((c) => !needle || `${text(c, "name")} ${connectionKind(c)} ${connectionHost(c)}`.toLowerCase().includes(needle));

  return (
    <>
      <StatRow cols={3}>
        <Stat label="Connections" icon={Cable} value={connections.data ? list.length : "—"} sub="databases the workspace reads" />
        <Stat label="Kinds" icon={Layers} value={connections.data ? kinds.size : "—"} sub={kinds.size ? [...kinds].map(kindLabel).join(", ") : "Postgres or Snowflake"} />
        <Stat
          label="Tables reachable"
          icon={Table2}
          value={connections.data ? (list.length ? reachable.value : 0) : "—"}
          sub={
            reachable.failed
              ? `${reachable.failed} connection${reachable.failed === 1 ? "" : "s"} did not answer`
              : reachable.warned
                ? `${reachable.warned} connection${reachable.warned === 1 ? " asks" : "s ask"} first (safe mode)`
                : "across every connection"
          }
          tone={reachable.failed ? "bad" : undefined}
        />
      </StatRow>
      {list.length > 0 && (
        <Toolbar>
          <SearchInput value={search} onChange={setSearch} placeholder="Search connections…" label="Search connections" />
        </Toolbar>
      )}
      <DataTable<Row>
        q={connections}
        what="connections"
        caption="Connections"
        rows={connections.data ? shown : undefined}
        rowKey={(c) => text(c, "name")}
        initialSort={{ key: "name", dir: "ascending" }}
        empty={
          list.length === 0 ? (
            <Empty icon={Cable} title="No connections yet" action={<Button variant="primary" onClick={onAdd}>Add connection</Button>}>
              Add the database your data lives in, or connect one of your services below. Its tables can then be loaded into datasets.
            </Empty>
          ) : (
            <Empty compact title="No connection matches">
              Clear the search to see every connection.
            </Empty>
          )
        }
        columns={[
          {
            key: "name",
            label: "Connection",
            sort: (c) => text(c, "name").toLowerCase(),
            render: (c) => (
              <div className="min-w-0">
                <Link to={connectionPath(text(c, "name"))} className="font-semibold">
                  {text(c, "name")}
                </Link>
              </div>
            ),
          },
          { key: "kind", label: "Kind", sort: connectionKind, render: (c) => <Chip>{kindLabel(connectionKind(c))}</Chip> },
          { key: "host", label: "Host", render: (c) => <Mono>{connectionHost(c) || "—"}</Mono> },
          {
            key: "service",
            label: "Service",
            render: (c) => {
              const service = serviceFor(c, serviceList);
              return service ? <Link to={`/services?${qs({ customer_id: scope.customerId })}`}>{text(service, "name")}</Link> : <span className="muted">—</span>;
            },
          },
          {
            key: "status",
            label: "Status",
            sort: (c) => connectionStatus(c).label,
            render: (c) => {
              const status = connectionStatus(c);
              return (
                <span title={status.detail}>
                  <Status value={status.label} tone={status.tone} />
                </span>
              );
            },
          },
          {
            key: "safe_mode",
            label: "Safe mode",
            render: (c) => <SafeModeControl variant="status" target={{ connection: text(c, "name") }} />,
          },
          {
            key: "created",
            label: "Created",
            sort: (c) => text(c, "created"),
            render: (c) => (
              <span className="font-mono text-[12px] text-ink-2" title={text(c, "created")}>
                {timeAgo(text(c, "created")) || "—"}
              </span>
            ),
          },
          {
            key: "actions",
            label: "",
            className: "text-right",
            render: (c) => <DeleteConnection name={text(c, "name")} />,
          },
        ]}
      />
      <div className="mt-6">
        <Query q={services} what="services" lines={1}>
          {(value) => <ConnectServices services={value.services ?? []} connections={list} />}
        </Query>
      </div>
    </>
  );
}

/** Delete a connection: the person types its name (`delete_connection`). */
export function DeleteConnection({ name, onDeleted, variant = "ghost" }: { name: string; onDeleted?: () => void; variant?: "ghost" | "danger" }) {
  const scope = useWorkspace();
  const toast = useToast();
  return (
    <ConfirmButton
      variant={variant}
      danger
      title={`Delete ${name}?`}
      confirmLabel="Delete"
      typed={name}
      confirm={<>This removes the connection {name} and its sealed credential from the workspace. Datasets already loaded from it stay.</>}
      onConfirm={async () => {
        await wa(scope, "delete_connection", { name });
        toast(`Deleted ${name}`);
        onDeleted?.();
      }}
    >
      Delete
    </ConfirmButton>
  );
}

/** Connect One of Your Services (lake#188): one click, no password typed or shown. */
function ConnectServices({ services, connections }: { services: Row[]; connections: Row[] }) {
  const scope = useWorkspace();
  const toast = useToast();
  const connect = useAsync();
  const names = new Set(connections.map((c) => text(c, "name")));
  const rows = services.filter((s) => text(s, "desired") !== "absent" && !s.default && text(s, "kind") !== "zetta");
  if (rows.length === 0) return null;
  return (
    <Card label="Your services" title="Connect One of Your Services">
      <p className="muted mt-0 mb-2 text-[12.5px]">The workspace reaches it inside the cluster, with the credential Lake already holds; nothing to type.</p>
      {connect.error ? <ErrorBlock error={connect.error} /> : null}
      <ul className="m-0 list-none divide-y divide-hairline p-0">
        {rows.map((service, i) => {
          const kind = text(service, "kind");
          const name = text(service, "name");
          const ready = text(service, "phase") === "Ready" && service.connected !== false;
          return (
            <li key={i} className="flex flex-wrap items-center gap-2 py-2">
              <span className="min-w-0 flex-1 font-semibold text-ink">{name}</span>
              <Chip>{kind}</Chip>
              {kind !== "postgres" ? (
                <span className="muted text-[12.5px]">The workspace can't read {kind} yet; Postgres works today</span>
              ) : !ready ? (
                <Status value="starting" />
              ) : (
                <Button
                  loading={connect.busy}
                  onClick={() =>
                    void connect.run(async () => {
                      await act("connect_service", { customer_id: scope.customerId, workspace_id: scope.workspaceId, service_id: text(service, "id") });
                      toast(`Connected ${name}`);
                    })
                  }
                >
                  {names.has(name) ? "Refresh" : "Connect"}
                </Button>
              )}
            </li>
          );
        })}
      </ul>
    </Card>
  );
}
