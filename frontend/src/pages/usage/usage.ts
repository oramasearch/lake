/** Usage over time (lake#564): the Usage dashboard's pure logic. The windows a person picks and the
 * one before each, how each dimension's unit reads, the change against the last period, readings of
 * several accounts added together, the stacked chart's series, the CSV export, limit amounts in a
 * person's units, and the links other pages make to Usage. `npm test`; the build runs it. */
import { bytes } from "../../lib/format.ts";

export type Granularity = "hour" | "day" | "month";

/** One breakdown row of a dimension: a workspace, a kind, an agent, a run or an object. */
export type Breakdown = { key: string; total: number; points: (number | null)[]; name?: string; workspace_id?: string; kind?: string; agent?: string };

export type Dimension = {
  dimension: string;
  title: string;
  unit: string;
  aggregation: "sum" | "last";
  total: number;
  points: (number | null)[];
  by_workspace: Breakdown[];
  by_kind: Breakdown[];
  by_agent?: Breakdown[];
  by_launch?: Breakdown[];
  by_model?: Breakdown[];
  by_object?: Breakdown[];
  /** GPU and GPU memory by GPU class (lake#643); "" is a GPU of no known class. */
  by_class?: Breakdown[];
};

export type Reading = {
  customer_id?: string;
  from: string;
  to: string;
  granularity: Granularity;
  periods: string[];
  dimensions: Dimension[];
  workspaces: { workspace_id: string; name: string; measured: boolean; message: string }[];
  not_measured: string[];
};

export type RangeKey = "24h" | "7d" | "30d" | "month" | "12m";

export const RANGES: { key: RangeKey; label: string; granularity: Granularity; previous: string }[] = [
  { key: "24h", label: "24 hours", granularity: "hour", previous: "the 24 hours before" },
  { key: "7d", label: "7 days", granularity: "day", previous: "the 7 days before" },
  { key: "30d", label: "30 days", granularity: "day", previous: "the 30 days before" },
  { key: "month", label: "This month", granularity: "day", previous: "last month" },
  { key: "12m", label: "12 months", granularity: "month", previous: "the 12 months before" },
];

export function rangeOf(value: string | null | undefined): RangeKey {
  return RANGES.some((r) => r.key === value) ? (value as RangeKey) : "month";
}

export type Window = { from: string; to: string; granularity: Granularity };

const HOUR = 3_600_000;
const DAY = 24 * HOUR;

const iso = (ms: number) => new Date(ms).toISOString().replace(/\.\d{3}Z$/, "Z");
const monthStart = (d: Date, shift = 0) => Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + shift, 1);

/** The window a range covers at `now` and the one just before it, in whole UTC periods. "This
 * month" is compared with all of last month. */
export function windows(key: RangeKey, now: Date = new Date()): { current: Window; previous: Window } {
  const t = now.getTime();
  const at = (from: number, to: number, granularity: Granularity): Window => ({ from: iso(from), to: iso(to), granularity });
  switch (key) {
    case "24h": {
      const end = Math.floor(t / HOUR) * HOUR + HOUR;
      return { current: at(end - 24 * HOUR, end, "hour"), previous: at(end - 48 * HOUR, end - 24 * HOUR, "hour") };
    }
    case "7d":
    case "30d": {
      const days = key === "7d" ? 7 : 30;
      const end = Math.floor(t / DAY) * DAY + DAY;
      return { current: at(end - days * DAY, end, "day"), previous: at(end - 2 * days * DAY, end - days * DAY, "day") };
    }
    case "12m":
      return { current: at(monthStart(now, -11), monthStart(now, 1), "month"), previous: at(monthStart(now, -23), monthStart(now, -11), "month") };
    case "month":
    default:
      return { current: at(monthStart(now), monthStart(now, 1), "day"), previous: at(monthStart(now, -1), monthStart(now), "day") };
  }
}

/** Seconds as hours, with one decimal under ten: "0.5 h", "12 h". */
function hours(seconds: number): string {
  const h = seconds / 3600;
  return h === 0 ? "0" : h < 10 ? h.toFixed(1).replace(/\.0$/, "") : Math.round(h).toLocaleString("en-US");
}

/** Resource-seconds (vCPU, GiB or GPU) by the minute under an hour, else by the hour: "30 vCPU-min",
 * "1.5 GiB-h". */
function held(seconds: number, what: string): string {
  return seconds < 3600 ? `${Math.round(seconds / 60).toLocaleString("en-US")} ${what}-min` : `${hours(seconds)} ${what}-h`;
}

function count(value: number): string {
  if (Math.abs(value) >= 1_000_000) return `${(value / 1_000_000).toFixed(1).replace(/\.0$/, "")}M`;
  if (Math.abs(value) >= 10_000) return `${(value / 1000).toFixed(1).replace(/\.0$/, "")}k`;
  return Math.round(value).toLocaleString("en-US");
}

/** A value in its dimension's unit as a person reads it. */
export function formatValue(value: number | null | undefined, unit: string): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "—";
  switch (unit) {
    case "bytes":
      return bytes(value);
    case "seconds":
      return value < 3600 ? `${Math.round(value / 60).toLocaleString("en-US")} min` : `${hours(value)} h`;
    case "instance_seconds":
      return `${hours(value)} instance-h`;
    case "gib_seconds":
      return `${hours(value)} GiB-h`;
    case "mcpu_seconds":
      return held(value / 1000, "vCPU");
    case "mib_seconds":
      return held(value / 1024, "GiB");
    case "gpu_seconds":
      return held(value, "GPU");
    case "tokens":
      return `${count(value)} tokens`;
    case "calls":
      return `${count(value)} calls`;
    default:
      return count(value);
  }
}

/** The change from the previous period: a percentage, or "new" when there was nothing before. */
export function change(current: number, previous: number | null | undefined): { text: string; direction: "up" | "down" | "flat" | "new" } {
  if (previous === null || previous === undefined) return { text: "", direction: "flat" };
  if (previous === 0) return current === 0 ? { text: "no change", direction: "flat" } : { text: "new this period", direction: "new" };
  const pct = ((current - previous) / previous) * 100;
  if (Math.abs(pct) < 0.5) return { text: "no change", direction: "flat" };
  return { text: `${pct > 0 ? "+" : ""}${pct.toFixed(Math.abs(pct) < 10 ? 1 : 0)}%`, direction: pct > 0 ? "up" : "down" };
}

function addLines(into: (number | null)[], line: (number | null)[]): (number | null)[] {
  const out = into.slice();
  line.forEach((value, i) => {
    if (value !== null && value !== undefined) out[i] = (out[i] ?? 0) + value;
  });
  return out;
}

function mergeBreakdowns(a: Breakdown[] = [], b: Breakdown[] = []): Breakdown[] {
  const rows = new Map<string, Breakdown>();
  for (const row of [...a, ...b]) {
    const found = rows.get(row.key);
    if (!found) rows.set(row.key, { ...row, points: row.points.slice() });
    else rows.set(row.key, { ...found, total: found.total + row.total, points: addLines(found.points, row.points) });
  }
  return [...rows.values()].sort((x, y) => y.total - x.total);
}

/** Readings of several accounts over the same window added together, dimension by dimension, the
 * way each reading adds up its own workspaces. A reading's latest-value total adds too, since each
 * account's workspaces are its own. */
export function mergeReadings(readings: Reading[]): Reading | null {
  const known = readings.filter(Boolean);
  if (!known.length) return null;
  const [first, ...rest] = known;
  const dimensions = new Map(first.dimensions.map((d) => [d.dimension, { ...d }]));
  for (const reading of rest) {
    for (const d of reading.dimensions) {
      const found = dimensions.get(d.dimension);
      if (!found) {
        dimensions.set(d.dimension, { ...d });
        continue;
      }
      dimensions.set(d.dimension, {
        ...found,
        total: found.total + d.total,
        points: addLines(found.points, d.points),
        by_workspace: mergeBreakdowns(found.by_workspace, d.by_workspace),
        by_kind: mergeBreakdowns(found.by_kind, d.by_kind),
        by_agent: d.by_agent || found.by_agent ? mergeBreakdowns(found.by_agent, d.by_agent) : undefined,
        by_launch: d.by_launch || found.by_launch ? mergeBreakdowns(found.by_launch, d.by_launch) : undefined,
        by_model: d.by_model || found.by_model ? mergeBreakdowns(found.by_model, d.by_model) : undefined,
        by_object: d.by_object || found.by_object ? mergeBreakdowns(found.by_object, d.by_object) : undefined,
        by_class: d.by_class || found.by_class ? mergeBreakdowns(found.by_class, d.by_class) : undefined,
      });
    }
  }
  return {
    ...first,
    customer_id: known.length === 1 ? first.customer_id : undefined,
    dimensions: [...dimensions.values()],
    workspaces: known.flatMap((r) => r.workspaces),
    not_measured: [...new Set(known.flatMap((r) => r.not_measured))],
  };
}

/** A breakdown's name for a person: a workspace's name, else the key in words. */
export function breakdownName(row: Breakdown, names: Record<string, string> = {}): string {
  if (row.name) return row.name;
  if (names[row.key]) return names[row.key];
  return KIND_NAMES[row.key] ?? row.key;
}

const KIND_NAMES: Record<string, string> = {
  repositories: "Repositories",
  images: "Container images",
  workspace_data: "Workspace data",
  transcripts: "Transcripts",
  drive_files: "Drive files",
  in: "Uploaded",
  out: "Downloaded",
  repository: "Repository builds",
  image: "Image builds",
  run: "Workspace runs",
  api: "API",
  mcp: "MCP",
  transcript: "Transcripts",
  input: "Input",
  output: "Output",
  service: "Services",
  store: "Stores",
  app: "Apps",
  zetta: "Workspace engine",
  workspace: "Workspace engines",
  build: "Image builds",
  agent: "Agents",
  postgres: "Postgres",
  kafka: "Kafka",
  clickhouse: "ClickHouse",
  tidb: "TiDB",
  cassandra: "Cassandra",
  valkey: "Valkey",
  sqlite: "SQLite",
  duckdb: "DuckDB",
  polars: "Polars",
  l4: "L4",
  a10g: "A10G",
  a100: "A100",
  h100: "H100",
  "": "Unknown class",
};

/** The stacked chart's series for one dimension, by workspace or by kind: the largest `top`
 * breakdowns and the rest as "Other", each stacked on the ones before (uPlot draws a stack from
 * running totals). `values` keeps each band's own value for the tooltip and the table. */
export function stackSeries(
  dimension: Dimension,
  by: "workspace" | "kind",
  names: Record<string, string> = {},
  top = 6,
): { labels: string[]; values: (number | null)[][]; stacked: (number | null)[][] } {
  const rows = by === "workspace" ? dimension.by_workspace : dimension.by_kind;
  const shown = rows.slice(0, top);
  const rest = rows.slice(top);
  const bands = shown.map((r) => ({ label: breakdownName(r, names), points: r.points }));
  if (rest.length) bands.push({ label: "Other", points: rest.reduce<(number | null)[]>((acc, r) => addLines(acc, r.points), new Array(dimension.points.length).fill(null)) });
  if (!bands.length) bands.push({ label: dimension.title, points: dimension.points });
  const values = bands.map((b) => b.points.map((v) => (v === null || v === undefined ? (dimension.aggregation === "last" ? null : 0) : v)));
  const stacked: (number | null)[][] = [];
  values.forEach((line, i) => {
    stacked.push(line.map((v, j) => (v === null ? (i ? stacked[i - 1][j] : null) : (i ? (stacked[i - 1][j] ?? 0) : 0) + v)));
  });
  return { labels: bands.map((b) => b.label), values, stacked };
}

function csvCell(value: unknown): string {
  const text = value === null || value === undefined ? "" : String(value);
  return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

/** The reading as CSV: one row per dimension, breakdown and period, in each dimension's own unit
 * (bytes, seconds, calls, tokens, instance-seconds, GiB-seconds, mCPU-seconds, MiB-seconds,
 * GPU-seconds), so a spreadsheet adds them up
 * exactly. */
export function toCsv(reading: Reading, names: Record<string, string> = {}): string {
  const rows: unknown[][] = [["dimension", "unit", "breakdown", "name", "period", "value"]];
  for (const d of reading.dimensions) {
    reading.periods.forEach((period, i) => rows.push([d.dimension, d.unit, "total", "", period, d.points[i] ?? ""]));
    for (const [breakdown, list] of [
      ["workspace", d.by_workspace],
      ["kind", d.by_kind],
      ["agent", d.by_agent ?? []],
      ["launch", d.by_launch ?? []],
      ["model", d.by_model ?? []],
      ["object", d.by_object ?? []],
      ["class", d.by_class ?? []],
    ] as const) {
      for (const row of list) {
        reading.periods.forEach((period, i) => rows.push([d.dimension, d.unit, breakdown, breakdown === "workspace" ? breakdownName(row, names) : row.key, period, row.points[i] ?? ""]));
      }
    }
  }
  return rows.map((r) => r.map(csvCell).join(",")).join("\n") + "\n";
}

/** A limit typed in a person's unit (GiB, hours, instance-hours, GiB-hours, vCPU-hours, GPU-hours,
 * a count) in the
 * dimension's own unit, and back. */
export function limitScale(unit: string): { label: string; factor: number } {
  switch (unit) {
    case "bytes":
      return { label: "GiB", factor: 1024 ** 3 };
    case "seconds":
      return { label: "hours", factor: 3600 };
    case "instance_seconds":
      return { label: "instance-hours", factor: 3600 };
    case "gib_seconds":
      return { label: "GiB-hours", factor: 3600 };
    case "mcpu_seconds":
      return { label: "vCPU-hours", factor: 3_600_000 };
    case "mib_seconds":
      return { label: "GiB-hours", factor: 3600 * 1024 };
    case "gpu_seconds":
      return { label: "GPU-hours", factor: 3600 };
    case "tokens":
      return { label: "tokens", factor: 1 };
    default:
      return { label: "calls", factor: 1 };
  }
}

export function toLimit(amount: string, unit: string): number | null {
  const text = amount.trim();
  if (!/^\d+(\.\d+)?$/.test(text)) return null;
  const value = Math.round(Number(text) * limitScale(unit).factor);
  return Number.isSafeInteger(value) ? value : null;
}

export function fromLimit(limit: number, unit: string): string {
  const value = limit / limitScale(unit).factor;
  return Number.isInteger(value) ? String(value) : value.toFixed(2).replace(/0+$/, "").replace(/\.$/, "");
}

/** A limit's meter tone: over is bad, a warning is the warning colour, otherwise none. */
export function limitTone(state: string): "bad" | "warn" | undefined {
  return state === "over" ? "bad" : state === "warning" ? "warn" : undefined;
}

/** What a limit does at its line: storage refuses repository writes, model tokens refuse agents'
 * model calls through Lake's model gateway (lake#248), every other limit warns. */
export function limitNote(limit: { dimension: string; enforced: boolean; warn_percent: number }): string {
  if (!limit.enforced) return `This month; warns from ${limit.warn_percent}%`;
  return limit.dimension === "model_tokens" ? "This month; agents' model calls are refused at the limit" : "Enforced on new repository writes";
}

/** A link to the Usage dashboard for some workspaces, a range and the dimension to chart. */
export function usageLink(opts: { workspaces?: string[]; range?: RangeKey; dimension?: string; customerId?: string } = {}): string {
  const params = new URLSearchParams();
  if (opts.customerId) params.set("customer_id", opts.customerId);
  if (opts.workspaces?.length) params.set("ws", opts.workspaces.join(","));
  if (opts.range && opts.range !== "month") params.set("range", opts.range);
  if (opts.dimension) params.set("dimension", opts.dimension);
  const query = params.toString().replace(/%2C/g, ",");
  return query ? `/usage?${query}` : "/usage";
}

/** What one thing used, from a reading of its workspace: per dimension, the total of the rows of
 * `breakdown` whose key is one of `keys` (an id, or an agent's name and id), in the dimension's
 * unit. A dimension the reading lacks counts nothing. */
export function usageOf(
  reading: Reading | null | undefined,
  match: { dimension: string; breakdown: "by_object" | "by_agent" | "by_workspace" | "by_model"; keys: string[] }[],
): { dimension: string; title: string; unit: string; value: number }[] {
  if (!reading) return [];
  return match.map((m) => {
    const d = reading.dimensions.find((x) => x.dimension === m.dimension);
    const rows = (d?.[m.breakdown] as Breakdown[] | undefined) ?? [];
    const keys = new Set(m.keys.filter(Boolean));
    return { dimension: m.dimension, title: d?.title ?? m.dimension, unit: d?.unit ?? "", value: rows.filter((r) => keys.has(r.key)).reduce((s, r) => s + r.total, 0) };
  });
}
