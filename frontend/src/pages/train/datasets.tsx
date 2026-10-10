/** Datasets as Train and Ship read them: the lean list, which tables are run outputs, and a list of
 * objects as a table. Shared by Data, Dataset, Query and Search Indexes. */
import type { Row } from "@/lib/api";
import { cellText, useSpaceRead } from "./workspace";

/** A database the engine writes run outputs to (datasets.cpp engine_database). */
export function engineDatabase(database: string): boolean {
  return (database.length > "_results".length && database.endsWith("_results")) || ["forecasts", "feedback", "system"].includes(database);
}

/** A table the engine wrote as a run's output (web_ui.cpp result_dataset). */
export function resultDataset(dataset: Row): boolean {
  return dataset.kind === "result" || (typeof dataset.database === "string" && engineDatabase(dataset.database));
}

/** The lean dataset list: `system.datasets` through workspace_sql (fast; Zetta's list_datasets is
 * slow). Annotate its rows with `annotateDatasets`. */
export function useLeanDatasets() {
  return useSpaceRead<{ rows?: Row[] }>("workspace_sql", { statement: "SELECT database, dataset FROM system.datasets" });
}

/** The rows as datasets::annotate_list leaves them: `name`, `id` and `kind` added. */
export function annotateDatasets(value: { rows?: Row[] } | undefined): Row[] {
  return (value?.rows ?? []).map((row) => {
    const database = typeof row.database === "string" ? row.database : "datasets";
    const name = typeof row.name === "string" ? row.name : typeof row.dataset === "string" ? row.dataset : "";
    return { ...row, name, ...(name && !row.id ? { id: `${database}/${name}` } : {}), kind: engineDatabase(database) ? "result" : "data" };
  });
}

/** A list of objects as a table, columns from the first row's keys (web_ui.cpp object_table). */
export function ObjectTable({ rows }: { rows: unknown }) {
  if (!Array.isArray(rows) || rows.length === 0) return null;
  const first = rows[0];
  if (!first || typeof first !== "object" || Array.isArray(first)) {
    return (
      <ul>
        {rows.map((row, i) => (
          <li key={i}>{cellText(row)}</li>
        ))}
      </ul>
    );
  }
  const columns = Object.keys(first as Row);
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            {columns.map((c) => (
              <th key={c}>{c}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row, i) => (
            <tr key={i}>
              {columns.map((c) => (
                <td key={c}>{row && typeof row === "object" && c in (row as Row) ? cellText((row as Row)[c]) : ""}</td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

