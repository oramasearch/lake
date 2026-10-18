/** The order of a table's columns (lake#812). A row arrives as a JSON object, and an object's keys
 * are not the table's order: the server writes them sorted, so a table built from them reads
 * alphabetically. `workspace_sql` answers `columns` in the order the query asked for (SELECT order,
 * or the table's own order for `SELECT *`), and `get_dataset` answers the table's `columns`; the
 * page follows those. Pure, tested by `tests/columns.test.ts`. */

type Row = Record<string, unknown>;

/** The columns a table shows: the `columns` the answer names, in that order, then any key a row
 * carries that they do not name. With no `columns`, the keys in the order the rows first carry
 * them. */
export function tableColumns(rows: readonly unknown[], columns?: unknown): string[] {
  const named = Array.isArray(columns) ? columns.filter((c): c is string => typeof c === "string") : [];
  const out: string[] = [];
  const seen = new Set<string>();
  const add = (name: string) => {
    if (seen.has(name)) return;
    seen.add(name);
    out.push(name);
  };
  for (const name of named) add(name);
  for (const row of rows) {
    if (!row || typeof row !== "object" || Array.isArray(row)) continue;
    for (const key of Object.keys(row as Row)) add(key);
  }
  return out;
}

/** A dataset's column names in the table's own order: those `get_dataset` lists, then those the
 * first rows' answer names, then any the analysis adds. */
export function datasetColumns(listed: unknown, sampled: unknown, analysed: unknown): string[] {
  const names = (value: unknown): string[] =>
    Array.isArray(value) ? value.map((c) => (typeof c === "string" ? c : c && typeof c === "object" && typeof (c as Row).name === "string" ? ((c as Row).name as string) : "")).filter(Boolean) : [];
  return tableColumns([], [...names(listed), ...names(sampled), ...names(analysed)]);
}
