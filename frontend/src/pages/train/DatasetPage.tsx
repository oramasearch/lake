/** Train → Dataset (web_ui.cpp workspace_content "dataset"): one dataset's facts, its columns with
 * their types, its first rows and its partitions. */
import { Link, useSearchParams } from "react-router-dom";
import type { Row } from "@/lib/api";
import { useTrail, entityCrumb } from "@/lib/trail";
import { Button, ErrorBlock, Skeleton } from "@/lib/ui";
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

function ColumnsTable({ dataset, analysis, rows }: { dataset: Row; analysis: Row; rows: Row[] }) {
  const names: string[] = ((dataset.columns ?? []) as unknown[]).map((c) => cellText(c));
  const analysed = new Map<string, Row>();
  for (const column of (analysis.columns ?? []) as Row[]) {
    const name = text(column, "name");
    analysed.set(name, column);
    if (!names.includes(name)) names.push(name);
  }
  if (names.length === 0) return <p className="muted">No columns yet.</p>;
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Column</th>
            <th>Type</th>
            <th>Reads as</th>
            <th>Empty</th>
          </tr>
        </thead>
        <tbody>
          {names.map((name) => {
            const column = analysed.get(name) ?? {};
            return (
              <tr key={name}>
                <td>
                  <code>{name}</code>
                </td>
                <td>{sampledType(rows, name)}</td>
                <td>{text(column, "semantic_type")}</td>
                <td>{typeof column.null_rate === "number" ? `${(column.null_rate * 100).toFixed(0)}%` : ""}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

export default function DatasetPage() {
  const [params] = useSearchParams();
  const name = params.get("name") ?? "";
  const database = params.get("database") || "datasets";
  const { customerId } = useWorkspace();
  const ws = useWsPath();
  const space = qs({ customer_id: customerId });
  useTrail([{ label: "Train", to: ws(`/train/data?${space}`) }, { label: "Data", to: ws(`/train/data?${space}`) }, ...(name ? [entityCrumb(name)] : [{ label: "Dataset" }])]);
  const error = params.get("error");
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title={name ? `Dataset: ${name}` : "Dataset"}
      before={
        error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null
      }
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
  return (
    <>
      <section className="panel">
        {(resultDataset(dataset) || engineDatabase(database)) && <p className="muted">A table the engine wrote as the output of a run.</p>}
        <dl>
          <dt>Id</dt>
          <dd>
            <code>{`${database}/${name}`}</code>
          </dd>
          <dt>Database</dt>
          <dd>{database}</dd>
          <dt>Stored as</dt>
          <dd>{stored}</dd>
          <dt>Rows</dt>
          <dd>{cellText(rows)}</dd>
          <dt>Bytes</dt>
          <dd>{text(dataset, "size_bytes")}</dd>
          <dt>Partitions</dt>
          <dd>{partitions.length}</dd>
          <dt>Copies</dt>
          <dd>{partitions.length === 0 ? "Every node" : copies}</dd>
          {dataset.degraded === true && (
            <>
              <dt>Health</dt>
              <dd className="error">{text(dataset, "degraded_reason")}</dd>
            </>
          )}
        </dl>
        {promote.error ? <ErrorBlock error={promote.error} /> : null}
        {partitions.length > 0 && copies <= 1 && (
          <>
            <Button
              loading={promote.busy}
              onClick={() =>
                void promote.run(async () => {
                  await wa(scope, "promote_dataset", { dataset: name, database });
                  toast(`Keeping ${name} durable`);
                })
              }
            >
              Keep durable
            </Button>{" "}
            <span className="muted">This dataset has one copy; keeping more lets it survive losing a node.</span>{" "}
          </>
        )}
        <Link to={ws(`/train/query?${qs({ customer_id: scope.customerId, statement: `SELECT * FROM ${database}.${name}` })}`)}>Query it</Link>
      </section>
      <section className="panel">
        <h2>Columns</h2>
        <ColumnsTable dataset={dataset} analysis={(analysis.data ?? {}) as Row} rows={sampleRows} />
      </section>
      {sample.data && (
        <section className="panel">
          <h2>First Rows</h2>
          <ObjectTable rows={sampleRows} />
        </section>
      )}
      {partitions.length > 0 && (
        <details className="panel">
          <summary>Storage details ({partitions.length} partitions)</summary>
          <ObjectTable rows={partitions} />
        </details>
      )}
    </>
  );
}
