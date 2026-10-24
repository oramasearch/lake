/** One connection (`/w/<id>/connections/connection?name=<name>`, lake#651): what it is, its source
 * tables with their columns (`list_source_tables`, nothing pulled), "Load into a dataset" for any
 * table or a query of one's own, and delete behind a typed confirmation. */
import { useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { Cable, Download, Table2 } from "lucide-react";
import type { Row } from "@/lib/api";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, Mono, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, text, timeAgo, useWq, useWsPath } from "@/pages/train/workspace";
import { DeleteConnection } from "./ConnectionsPage";
import { LoadFromConnection } from "./forms";
import { SafeModeControl, useSafeModeRead } from "./SafeMode";
import { CONNECTIONS_PATH, connectionHost, connectionKind, connectionStatus, datasetNameFor, kindLabel, tableQuery } from "./connections";

export default function ConnectionPage() {
  const [params] = useSearchParams();
  const name = params.get("name") ?? "";
  const ws = useWsPath();
  useTrail([{ label: "Data", to: "/repositories" }, { label: "Connections", to: ws(CONNECTIONS_PATH) }, ...(name ? [entityCrumb(name)] : [])]);
  return (
    <WorkspaceFrame eyebrow="Connection" title={name || "Connection"} sub="A database this workspace reads, and the tables it can load from it.">
      {() => <ConnectionContent name={name} />}
    </WorkspaceFrame>
  );
}

function ConnectionContent({ name }: { name: string }) {
  const ws = useWsPath();
  const navigate = useNavigate();
  const connections = useWq<{ connections?: Row[] }>("list_connections");
  const list = connections.data?.connections ?? [];
  const connection = list.find((c) => text(c, "name") === name);
  // Listing the tables runs a catalog query on the connection, so its safe mode may ask first
  // (lake#906): Run lists them with the warning's confirmation.
  const [confirmed, setConfirmed] = useState<string | undefined>(undefined);
  const tables = useWq<{ tables?: Row[]; truncated?: boolean }>(
    connection ? "list_source_tables" : null,
    { connection: name, include_columns: true, max_tables: 200, ...(confirmed ? { safe_mode_confirmation: confirmed } : {}) },
    { retry: false },
  );
  const safe = useSafeModeRead(tables.error, name);
  if (safe.confirmation !== confirmed) setConfirmed(safe.confirmation);
  const [search, setSearch] = useState("");
  const [loading, setLoading] = useState<{ query?: string; tableName?: string } | null>(null);

  if (connections.isLoading) return <Skeleton lines={4} />;
  if (!connection) {
    return (
      <Empty icon={Cable} title={name ? `No connection named ${name}` : "No connection chosen"} action={<Button to={ws(CONNECTIONS_PATH)}>All connections</Button>}>
        It may have been deleted. Connections lists the ones this workspace has.
      </Empty>
    );
  }
  const kind = connectionKind(connection);
  const status = connectionStatus(connection);
  const connector = (connection.connector ?? {}) as Row;
  const all = tables.data?.tables ?? [];
  const needle = search.trim().toLowerCase();
  const shown = all.filter((t) => !needle || `${text(t, "schema")}.${text(t, "table")}`.toLowerCase().includes(needle));

  return (
    <>
      <StatRow cols={3}>
        <Stat label="Kind" icon={Cable} value={kindLabel(kind)} sub={connectionHost(connection) || "—"} />
        <Stat label="Tables" icon={Table2} value={tables.data ? `${all.length}${tables.data.truncated ? "+" : ""}` : tables.isError ? "—" : "…"} sub={tables.isError ? "the database did not answer" : "this connection can read"} tone={tables.isError ? "bad" : undefined} />
        <Stat label="Status" value={<Status value={status.label} tone={status.tone} />} sub={status.detail || `created ${timeAgo(text(connection, "created")) || "—"}`} />
      </StatRow>

      <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_18rem]">
        <section aria-label="Source tables" className="min-w-0">
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search tables…" label="Search tables" />
            <Button onClick={() => setLoading({})}>
              <Download size={14} aria-hidden /> Load with a query
            </Button>
          </Toolbar>
          {safe.notice}
          {safe.dialog}
          {tables.data?.truncated && <Banner tone="quiet">Showing the first {all.length} tables. Search narrows them; Load with a query reaches any table.</Banner>}
          <DataTable<Row>
            q={tables}
            what="tables"
            caption={`Tables in ${name}`}
            rows={tables.data ? shown : undefined}
            rowKey={(t) => `${text(t, "schema")}.${text(t, "table")}`}
            initialSort={{ key: "table", dir: "ascending" }}
            empty={
              all.length === 0 ? (
                <Empty compact icon={Table2} title="No tables">
                  This connection shows no tables to its account.
                </Empty>
              ) : (
                <Empty compact title="No table matches">
                  Clear the search to see every table.
                </Empty>
              )
            }
            columns={[
              {
                key: "table",
                label: "Table",
                sort: (t) => `${text(t, "schema")}.${text(t, "table")}`,
                render: (t) => (
                  <div className="min-w-0">
                    <div className="font-semibold text-ink">{text(t, "table")}</div>
                    <div className="text-[12px] text-ink-3">{text(t, "schema")}</div>
                  </div>
                ),
              },
              { key: "kind", label: "Kind", render: (t) => (text(t, "kind") ? <Chip>{text(t, "kind")}</Chip> : "—") },
              {
                key: "columns",
                label: "Columns",
                render: (t) => (
                  <span className="text-[12.5px] text-ink-2">
                    {((t.columns ?? []) as Row[]).map((c) => `${text(c, "column")} ${text(c, "type")}`).join(", ") || "—"}
                  </span>
                ),
              },
              {
                key: "load",
                label: "",
                className: "text-right",
                render: (t) => (
                  <Button variant="ghost" onClick={() => setLoading({ query: tableQuery(text(t, "schema"), text(t, "table"), kind), tableName: datasetNameFor(text(t, "table")) })}>
                    Load into a dataset
                  </Button>
                ),
              },
            ]}
          />
        </section>
        <aside className="min-w-0">
          <Card label="Connection" title={name}>
            <dl className="m-0 grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-[12.5px]">
              {Object.entries(connector)
                .filter(([key]) => key !== "type")
                .map(([key, value]) => (
                  <div key={key} className="contents">
                    <dt className="text-ink-3">{key}</dt>
                    <dd className="m-0 min-w-0 truncate">
                      <Mono>{typeof value === "string" ? value : JSON.stringify(value)}</Mono>
                    </dd>
                  </div>
                ))}
              <dt className="text-ink-3">created</dt>
              <dd className="m-0 font-mono" title={text(connection, "created")}>
                {timeAgo(text(connection, "created")) || "—"}
              </dd>
            </dl>
            <p className="muted mb-0 text-[12px]">The password or token is sealed in the workspace and never shown.</p>
            <div className="mt-3 border-t border-hairline pt-3">
              <div className="label mb-1.5">Safe mode</div>
              <SafeModeControl target={{ connection: name }} />
              <p className="muted mt-1.5 mb-0 text-[12px]">Whether Lake shows the exact query and asks before it runs against this connection.</p>
            </div>
            <div className="mt-3 border-t border-hairline pt-3">
              <DeleteConnection name={name} variant="danger" onDeleted={() => navigate(ws(CONNECTIONS_PATH))} />
            </div>
          </Card>
        </aside>
      </div>
      {loading && <LoadFromConnection connections={list} initial={{ connection: name, ...loading }} onClose={() => setLoading(null)} />}
    </>
  );
}
