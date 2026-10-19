/** The order of a table's columns (lake#812), their types (zetta#1021), and the first rows by
 * `LIMIT`. A row arrives as a JSON object, and an object's keys are not the table's order: the
 * server writes them sorted, so a table built from them reads alphabetically. `workspace_sql`
 * answers `columns` in the order the query asked for (SELECT order, or the table's own order for
 * `SELECT *`), and `get_dataset` answers the table's `columns`; the page follows those. An engine
 * with zetta#1021 keeps a loaded file's own column order and answers `get_dataset`'s columns typed,
 * `[{name, type, nullable, null_fraction}]`; an older one answers names. Both are read here. Pure,
 * tested by `tests/columns.test.ts`. */

type Row = Record<string, unknown>;

/** Column names from a list of names or of `{name, …}` objects (get_dataset's typed columns). */
function names(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((c) => (typeof c === "string" ? c : c && typeof c === "object" && typeof (c as Row).name === "string" ? ((c as Row).name as string) : ""))
    .filter(Boolean);
}

/** The columns a table shows: the `columns` the answer names, in that order, then any key a row
 * carries that they do not name. With no `columns`, the keys in the order the rows first carry
 * them. */
export function tableColumns(rows: readonly unknown[], columns?: unknown): string[] {
  const named = names(columns);
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
  return tableColumns([], [...names(listed), ...names(sampled), ...names(analysed)]);
}

/** One column as `get_dataset` types it over the whole dataset (zetta#1021). */
export type ColumnFacts = { type: string; nullable: boolean; nullFraction: number | null };

/** get_dataset's typed columns by name. Empty when the engine answers names only (before
 * zetta#1021): the page then reads types from the first rows and null shares from the analysis. */
export function columnFacts(listed: unknown): Map<string, ColumnFacts> {
  const facts = new Map<string, ColumnFacts>();
  if (!Array.isArray(listed)) return facts;
  for (const column of listed) {
    if (!column || typeof column !== "object") continue;
    const { name, type, nullable, null_fraction } = column as Row;
    if (typeof name !== "string" || typeof type !== "string") continue;
    facts.set(name, { type, nullable: nullable === true, nullFraction: typeof null_fraction === "number" ? null_fraction : null });
  }
  return facts;
}

/** The engine's JSON type in the words the Columns tab uses for a sampled type. */
export function typeLabel(type: string): string {
  switch (type) {
    case "integer":
    case "boolean":
    case "mixed":
    case "unknown":
      return type;
    case "number":
      return "decimal";
    case "string":
      return "text";
    case "object":
    case "array":
      return "JSON";
    case "null":
      return "empty";
    default:
      return type;
  }
}

/** Whether the engine refused `LIMIT`: one older than zetta#1039 stops at it, answering
 * `expected ';' after statement, got 'LIMIT'`. The page then asks without it and keeps the first
 * rows itself. Any other error is the query's own. */
export function limitRefused(error: unknown): boolean {
  const message = error instanceof Error ? error.message : typeof error === "string" ? error : error && typeof error === "object" ? String((error as Row).message ?? "") : "";
  return /got '(limit|fetch)'/i.test(message);
}

/** The most rows a page reads: `rows` cut to `limit`. With `LIMIT` the engine already sent no more
 * (zetta#1021), so this changes nothing; against an engine that refused it, it is the trimming. */
export function firstRows<T>(rows: readonly T[] | undefined, limit: number): T[] {
  return (rows ?? []).slice(0, limit);
}

/** A Query page statement with `LIMIT cap + 1` appended, so the engine sends at most one row more
 * than the page shows (enough to say there are more) instead of the whole table. Only a plain
 * `SELECT` that names no `LIMIT`, `FETCH` or `OFFSET` of its own is changed; anything else is sent
 * as written. Quoted text is ignored when looking. After a `--` comment the `LIMIT` goes on a line
 * of its own, so the comment does not swallow it. */
export function cappedStatement(statement: string, cap: number): string {
  const body = statement.trim().replace(/;\s*$/, "");
  if (!/^select\b/i.test(body)) return statement;
  const unquoted = body.replace(/'(?:[^']|'')*'/g, "''").replace(/"(?:[^"]|"")*"/g, '""');
  if (/\b(limit|fetch|offset)\b/i.test(unquoted)) return statement;
  if (unquoted.includes(";")) return statement;
  return `${body}${unquoted.includes("--") ? "\n" : " "}LIMIT ${cap + 1}`;
}
