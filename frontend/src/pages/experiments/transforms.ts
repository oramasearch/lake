/** The experiment pages' data transforms, ported from `assets/experiments/experiments.js`
 * unchanged in behaviour: EMA smoothing, metric alignment by step or time, parameter
 * differences, the live cursor, and metric labels. */

export type Point = { step: number; value: number; time: string };
export type Series = { name: string; points: Point[] };
export type RunDetail = {
  run: string;
  params?: Record<string, unknown>;
  summary?: Record<string, { final?: unknown } | undefined>;
  series?: Series[];
  artifacts?: Artifact[];
  metrics_suppressed?: string;
};
export type Artifact = { name: string; type: string; data: Record<string, unknown> & Record<string, unknown[]> };

/** Exponential moving average: `w * previous + (1 - w) * value`. */
export function ema(points: Point[], weight: number): Point[] {
  let previous: number | undefined;
  return points.map((point) => {
    const value = previous === undefined ? point.value : weight * previous + (1 - weight) * point.value;
    previous = value;
    return { ...point, value };
  });
}

/** One metric across runs as uPlot columns: the union of x values, then each run's y (null where
 * the run has no point; non-positive values are null on a log axis). */
export function metricData(details: RunDetail[], name: string, axis: string, weight: number, logarithmic: boolean): (number | null)[][] {
  const columns = details.map((run) => {
    if (run.metrics_suppressed) return new Map<number, number | null>();
    const series = run.series ?? [];
    const times = series.flatMap((s) => s.points.map((point) => Date.parse(point.time))).filter(Number.isFinite);
    const start = times.length ? times.reduce((a, b) => Math.min(a, b)) : 0;
    const points = [...(series.find((s) => s.name === name)?.points ?? [])].sort((a, b) => a.step - b.step);
    return new Map<number, number | null>(
      ema(points, weight)
        .map((point): [number, number | null] => {
          const x = axis === "step" ? point.step : (Date.parse(point.time) - (axis === "relative" ? start : 0)) / 1000;
          const value = logarithmic && point.value <= 0 ? null : point.value;
          return [x, value];
        })
        .filter(([x]) => Number.isFinite(x)),
    );
  });
  const x = [...new Set(columns.flatMap((column) => [...column.keys()]))].sort((a, b) => a - b);
  return [x, ...columns.map((column) => x.map((at) => (column.has(at) ? column.get(at)! : null)))];
}

export type ParameterRow = { name: string; values: string[]; differs: boolean };

/** Each parameter's value per run, and whether the runs differ on it. */
export function parameterDiff(details: RunDetail[]): ParameterRow[] {
  const names = [...new Set(details.flatMap((run) => Object.keys(run.params ?? {})))].sort();
  return names.map((name) => {
    const values = details.map((run) => (run.params && Object.hasOwn(run.params, name) ? JSON.stringify(run.params[name]) : "—"));
    return { name, values, differs: new Set(values).size > 1 };
  });
}

/** The next live cursor: after a nonempty batch's last event, never the allocator high-water mark;
 * an empty batch's `next_sequence` otherwise. */
export function nextCursor(batch: { events: { sequence: number }[]; next_sequence: number }, previous: number): number {
  if (batch.events.length) return Math.max(previous, ...batch.events.map((event) => event.sequence + 1));
  return Math.max(previous, batch.next_sequence);
}

/** Zetta's held-out metric names and the spellings it accepts as aliases (web_ui_experiments.cpp
 * metric_names, zetta#1217). */
export const METRIC_NAMES: [string, string[]][] = [
  ["held_out_auc", ["validation.auc", "ROC-AUC", "held-out ROC-AUC", "roc_auc", "auc"]],
  ["held_out_log_loss", ["validation.log_loss", "log loss", "log_loss", "logloss"]],
  ["held_out_accuracy", ["validation.accuracy", "accuracy"]],
  ["held_out_rmse", ["validation.rmse", "RMSE", "rmse"]],
  ["held_out_mae", ["validation.mae", "MAE", "mae"]],
];

function metricKey(name: string): string {
  return name.toLowerCase().replace(/[ .-]/g, "_");
}

const ALIASES: Record<string, string> = Object.fromEntries(
  METRIC_NAMES.flatMap(([canonical, spellings]) => [[metricKey(canonical), canonical], ...spellings.map((spelling) => [metricKey(spelling), canonical])]),
);

/** A metric's canonical name, for a study recorded under an older spelling. */
export function metricLabel(name: unknown): string {
  if (typeof name !== "string") return JSON.stringify(name);
  return ALIASES[metricKey(name)] ?? name;
}

/** A recorded value as text: strings as they are, everything else as JSON; absent reads "—". */
export function cell(value: unknown): string {
  if (value === undefined || value === null) return "—";
  return typeof value === "string" ? value : JSON.stringify(value);
}
