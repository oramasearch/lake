/** Data → Dataset (web_ui.cpp workspace_content "dataset"): one dataset's figures, its columns
 * with their types, its first rows and its partitions beside its facts and actions. */
import { useState } from "react";
import { useSearchParams } from "react-router-dom";
import { Columns3, Database, HardDrive, Rows3, Shield, Terminal } from "lucide-react";
import type { Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { useTrail, entityCrumb } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, KeyValue, Mono, Skeleton, Split, Stat, StateTabs, StatRow, Status } from "@/lib/ui";
import { useToast } from "@/lib/toast";
import { WorkspaceFrame, cellText, qs, text, useAsync, useSpaceRead, useWorkspace, useWq, useWsPath, wa } from "./workspace";
import { ObjectTable, engineDatabase, resultDataset } from "./datasets";

/** The value type SQL sees in a column, read from the first rows (web_ui.cpp sampled_type). */
function sampledType(rows: Row[], column: string): string {
  const seen = new Set<string>();
  for (const row of rows) {
    if (!row || typeof row !== "object" || !(column in row) || row[column] === null) continue;
    const value = row[column];
    seen.add(
      typeof value === "boolean" ? "boolean" : typeof value === "number" ? (Number.isInteger(value) ? "integer" : "decimal") : typeof value === "string" ? "text" : "JSON",
    );
  }
  if (seen.size === 0) return "empty";
  if (seen.size === 2 && seen.has("integer") && seen.has("decimal")) return "decimal";
  if (seen.size > 1) return [...seen].sort().join(" or ");
  return [...seen][0];
}


/** The dataset's column names: those it lists, then any the analysis adds. */
function columnNames(dataset: Row, analysis: Row): string[] {
  const names: string[] = ((dataset.columns ?? []) as unknown[]).map((c) => cellText(c));
  for (const column of (analysis.columns ?? []) as Row[]) {
    const name = text(column, "name");
    if (!names.includes(name)) names.push(name);
  }
  return names;
}

function ColumnsTable({ names, analysis, rows }: { names: string[]; analysis: Row; rows: Row[] }) {
  const analysed = new Map<string, Row>();
  for (const column of (analysis.columns ?? []) as Row[]) analysed.set(text(column, "name"), column);
  if (names.length === 0)
    return (
      <Empty compact icon={Columns3} title="No columns yet">
        The dataset lists no columns.
      </Empty>
    );
  return (
    <DataTable<string>
      rows={names}
      what="columns"
      rowKey={(name) => name}
      columns={[
        { key: "column", label: "Column", sort: (name) => name, render: (name) => <Mono>{name}</Mono> },
        { key: "type", label: "Type", sort: (name) => sampledType(rows, name), render: (name) => <Chip>{sampledType(rows, name)}</Chip> },
        { key: "semantic", label: "Reads as", render: (name) => text(analysed.get(name) ?? {}, "semantic_type") || <span className="muted">—</span> },
        {
          key: "empty",
          label: "Empty",
          numeric: true,
          sort: (name) => {
            const rate = (analysed.get(name) ?? {}).null_rate;
            return typeof rate === "number" ? rate : null;
          },
          render: (name) => {
            const rate = (analysed.get(name) ?? {}).null_rate;
            return typeof rate === "number" ? <span className="font-mono">{`${(rate * 100).toFixed(0)}%`}</span> : "";
          },
        },
      ]}
    />
  );
}

export default function DatasetPage() {
  const [params] = useSearchParams();
  const name = params.get("name") ?? "";
  const database = params.get("database") || "datasets";
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([{ label: "Data", to: "/repositories" }, { label: "Datasets", to: ws(`/train/data?${space}`) }, ...(name ? [entityCrumb(name)] : [{ label: "Dataset" }])]);
  const error = params.get("error");
  return (
    <WorkspaceFrame
      eyebrow="Data"
      title={
        name ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {name}
            {engineDatabase(database) ? <Chip tone="ai">Run output</Chip> : null}
          </span>
        ) : (
          "Dataset"
        )
      }
      sub={name ? <span className="font-mono text-[12.5px]">{`${database}/${name}`}</span> : undefined}
      actions={
        name ? (
          <Button to={ws(`/train/query?${qs({ customer_id: customerId, statement: `SELECT * FROM ${database}.${name}` })}`)}>
            <Terminal size={14} aria-hidden /> Query it
          </Button>
        ) : null
      }
      before={error ? <Banner tone="warn">{error}</Banner> : null}
    >
      {() => <DatasetContent name={name} database={database} />}
    </WorkspaceFrame>
  );
}

function DatasetContent({ name, database }: { name: string; database: string }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const toast = useToast();
  const promote = useAsync();
  const [tab, setTab] = useState<"columns" | "rows" | "partitions">("columns");
  const got = useWq<Row>("get_dataset", { dataset: name, database });
  // The first rows and the analysis are extras: when either fails the page shows what it has.
  const sample = useSpaceRead<{ rows?: Row[] }>("workspace_sql", { statement: `SELECT * FROM ${database}.${name} LIMIT 20` });
  const analysis = useWq<Row>("analyze_dataset", { dataset: `${database}.${name}`, sample_rows: 256 });
  if (got.isLoading) return <Skeleton />;
  if (got.isError) return <ErrorBlock error={got.error} what="the dataset" retry={() => void got.refetch()} />;
  const dataset = (got.data ?? {}) as Row;
  const partitions = (dataset.partitions ?? []) as Row[];
  const sampleRows = ((sample.data?.rows ?? []) as Row[]).slice(0, 20);
  const stored = text(dataset, "format") === "jsonl" ? "JSON rows" : text(dataset, "format");
  // The fewest copies any partition has (web_ui.cpp fewest_copies).
  let copies = 0;
  for (const partition of partitions) {
    const owners = ((partition.owner_nodes ?? []) as unknown[]).length;
    copies = copies === 0 ? owners : Math.min(copies, owners);
  }
  const rows = dataset.rows ?? dataset.expected_rows ?? "";
  const names = columnNames(dataset, (analysis.data ?? {}) as Row);
  const size = text(dataset, "size_bytes");
  const single = partitions.length > 0 && copies <= 1;
  return (
    <>
      {(resultDataset(dataset) || engineDatabase(database)) && <Banner tone="quiet">A table the engine wrote as the output of a run.</Banner>}
      {dataset.degraded === true && <Banner tone="warn" title="This dataset is degraded">{text(dataset, "degraded_reason")}</Banner>}
      <StatRow>
        <Stat label="Rows" icon={Rows3} value={typeof rows === "number" ? rows.toLocaleString("en-US") : cellText(rows) || "—"} />
        <Stat label="Size" icon={HardDrive} value={size ? bytes(size) : "—"} sub={size ? `${Number(size).toLocaleString("en-US")} bytes` : undefined} />
        <Stat label="Columns" icon={Columns3} value={names.length} />
        <Stat label="Copies" icon={Shield} value={partitions.length === 0 ? "All" : copies} sub={partitions.length === 0 ? "every node" : `${partitions.length} partitions`} tone={single ? "bad" : undefined} />
      </StatRow>
      <Split
        side={
          <>
            <Card label="Facts">
              <KeyValue
                rows={[
                  ["Id", <Mono key="id">{`${database}/${name}`}</Mono>],
                  ["Database", database],
                  ["Stored as", stored],
                  ["Rows", cellText(rows)],
                  ["Bytes", size],
                  ["Partitions", partitions.length],
                  ["Copies", partitions.length === 0 ? "Every node" : copies],
                  ...(dataset.degraded === true ? ([["Health", <Status key="health" value="degraded" tone="danger" />]] as [string, React.ReactNode][]) : []),
                ]}
              />
            </Card>
            <Card label="Actions">
              <div className="flex flex-col gap-2">
                <Button to={ws(`/train/query?${qs({ customer_id: scope.customerId, statement: `SELECT * FROM ${database}.${name}` })}`)}>
                  <Terminal size={14} aria-hidden /> Query it
                </Button>
                {single && (
                  <>
                    {promote.error ? <ErrorBlock error={promote.error} /> : null}
                    <Button
                      variant="primary"
                      loading={promote.busy}
                      onClick={() =>
                        void promote.run(async () => {
                          await wa(scope, "promote_dataset", { dataset: name, database });
                          toast(`Keeping ${name} durable`);
                        })
                      }
                    >
                      <Shield size={14} aria-hidden /> Keep durable
                    </Button>
                    <p className="muted m-0 text-[12.5px]">This dataset has one copy; keeping more lets it survive losing a node.</p>
                  </>
                )}
                <Button variant="ghost" to={ws(`/train/data?${qs({ customer_id: scope.customerId })}`)}>
                  <Database size={14} aria-hidden /> All datasets
                </Button>
              </div>
            </Card>
          </>
        }
      >
        <Card padded={false}>
          <div className="px-4 pt-3">
            <StateTabs
              label="Dataset"
              value={tab}
              onChange={setTab}
              tabs={[
                { value: "columns", label: `Columns (${names.length})` },
                { value: "rows", label: "First Rows" },
                ...(partitions.length > 0 ? [{ value: "partitions" as const, label: `Partitions (${partitions.length})` }] : []),
              ]}
            />
          </div>
          <div className="px-4 pb-4">
            {tab === "columns" && <ColumnsTable names={names} analysis={(analysis.data ?? {}) as Row} rows={sampleRows} />}
            {tab === "rows" &&
              (sample.isLoading ? (
                <Skeleton />
              ) : sample.isError ? (
                <ErrorBlock error={sample.error} what="the first rows" retry={() => void sample.refetch()} />
              ) : sampleRows.length === 0 ? (
                <Empty compact icon={Rows3} title="No rows">
                  The dataset holds no rows yet.
                </Empty>
              ) : (
                <ObjectTable rows={sampleRows} />
              ))}
            {tab === "partitions" && <ObjectTable rows={partitions} />}
          </div>
        </Card>
      </Split>
    </>
  );
}
