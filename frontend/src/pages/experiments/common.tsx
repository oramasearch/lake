/** What the four Experiments pages share (web_ui_experiments.cpp and experiments.js): the shell
 * (heading, trail, notice, Live), the capability client, tables, the confirmed actions, the live
 * follow loop and the metric charts with their toolbar. */
import { type ReactNode, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { act, ApiError, postRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Page } from "@/lib/ui";
import { pause, qs, useWorkspaceLink } from "@/pages/apps/shared";
import { useConfirm } from "@/pages/streams/common";
import "@/styles/experiments.css";
import { Chart, ChartContext, newRegistry, viewAction } from "./Chart";
import { cell, ema, metricData, nextCursor, parameterDiff, type RunDetail } from "./transforms";

export type Mode = "list" | "study" | "run" | "compare";

const TITLES: Record<Mode, string> = { list: "Experiments", study: "Study", run: "Run Charts", compare: "Compare Runs" };

/** An error as the page's notice reads it: the field Zetta names, then its message (zetta#1217). */
export function describe(error: unknown): string {
  if (error instanceof ApiError) return (error.path ? `${error.path}: ` : "") + error.message;
  return error instanceof Error ? error.message : String(error);
}

/** The experiment capabilities, called as the page's script called them: POST with the space and
 * workspace. Reads go through `postRead`, which refreshes nothing. */
export function useExperimentClient() {
  const { space, workspaceId } = useSession();
  const customerId = space?.id ?? "";
  return useMemo(
    () => ({
      customerId,
      workspaceId,
      read: <T = Row,>(capability: string, input: Row = {}) => postRead<T>(capability, { customer_id: customerId, ...input, workspace_id: workspaceId }),
      act: <T = Row,>(capability: string, input: Row = {}) => act<T>(capability, { customer_id: customerId, ...input, workspace_id: workspaceId }),
    }),
    [customerId, workspaceId],
  );
}

/** A page address under the workspace with the space's id, as the script built it. */
export function useExperimentUrl() {
  const ws = useWorkspaceLink();
  const { space } = useSession();
  return useCallback((path: string, input: Record<string, string> = {}) => ws(path) + qs({ customer_id: space?.id, ...input }), [ws, space]);
}

/** The heading every Experiments page shares; non-list pages add All experiments and Live. */
export function ExperimentsShell({
  mode,
  notice,
  live,
  onLive,
  status,
  children,
}: {
  mode: Mode;
  notice: string;
  live?: boolean;
  onLive?: (value: boolean) => void;
  status?: string;
  children: ReactNode;
}) {
  const ws = useWorkspaceLink();
  const url = useExperimentUrl();
  useTrail([
    { label: "Train", to: ws("/train/runs") },
    { label: "Experiments", to: ws("/train/experiments") },
  ]);
  return (
    <Page eyebrow="Train" title={TITLES[mode]} sub="Follow training, compare results, and promote a completed trial.">
      <div className="experiments">
        <p role="status">{notice}</p>
        {mode !== "list" && (
          <>
            <p>
              <Link to={url("/train/experiments", { project: "" })}>All experiments</Link>
            </p>
            <label>
              <input type="checkbox" checked={!!live} onChange={(e) => onLive?.(e.target.checked)} /> Live updates
            </label>
            <span role="status">{status}</span>
          </>
        )}
        {children}
      </div>
    </Page>
  );
}

/** A table of recorded values, as the script's `table()` drew it. */
export function Table({ headers, rows, caption }: { headers: ReactNode[]; rows: unknown[][]; caption?: string }) {
  return (
    <div className="table-scroll">
      <table>
        {caption && <caption>{caption}</caption>}
        <thead>
          <tr>
            {headers.map((header, index) => (
              <th key={index} scope="col">
                {header}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row, index) => (
            <tr key={index}>
              {row.map((value, column) => (
                <td key={column}>{cell(value)}</td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

/** Stop a study, stop a trial or promote a trial, each confirmed in words with its input shown. */
export function useExperimentActions(onDone: () => Promise<unknown> | void, setNotice: (text: string) => void) {
  const client = useExperimentClient();
  const [dialog, confirm] = useConfirm();
  const [busy, setBusy] = useState<string | null>(null);
  const perform = useCallback(
    async (capability: "stop_study" | "stop_experiment_trial" | "promote_trial", study: string, run?: string) => {
      const input: Row = { study };
      if (run) input.run_id = run;
      const message =
        capability === "promote_trial"
          ? "Promote this completed trial’s registered model as the study’s project model version?"
          : "Stop this training work? Active trials will be ended cooperatively.";
      if (!(await confirm(message, "Confirm", JSON.stringify(input, null, 2)))) return;
      const key = `${capability}:${study}:${run ?? ""}`;
      setBusy(key);
      try {
        const result = await client.act<Row>(capability, { ...input, confirm: true });
        setNotice(capability === "promote_trial" ? `Promoted model ${String(result.model ?? "")}` : "Training stop requested.");
        await onDone();
      } catch (error) {
        setNotice(describe(error));
      } finally {
        setBusy(null);
      }
    },
    [client, confirm, onDone, setNotice],
  );
  return { dialog, confirm, perform, busy };
}

export type Target = { kind: "study" | "run"; id: string };

/** Live updates (experiments.js follow): while Live is on and the tab is visible, long-poll
 * `tail_experiment_events` for each target, refresh on any event and once more per cycle, at most
 * every 10 seconds and at least 1 second apart. */
export function useFollow(live: boolean, targets: () => Target[], refresh: () => Promise<void>) {
  const client = useExperimentClient();
  const [status, setStatus] = useState("Connecting…");
  const liveRef = useRef(live);
  liveRef.current = live;
  const targetsRef = useRef(targets);
  targetsRef.current = targets;
  const refreshRef = useRef(refresh);
  refreshRef.current = refresh;
  useEffect(() => {
    const controller = new AbortController();
    const signal = controller.signal;
    const cursors = new Map<string, number>();
    void (async () => {
      while (!signal.aborted) {
        const began = Date.now();
        if (!liveRef.current || document.hidden) {
          setStatus("Paused");
          await pause(1000, signal);
          continue;
        }
        try {
          await Promise.all(
            targetsRef.current().map(async (target) => {
              const key = `${target.kind}:${target.id}`;
              const previous = cursors.get(key) ?? 1;
              const batch = await client.read<{ events: { sequence: number }[]; next_sequence: number }>("tail_experiment_events", {
                ...target,
                from_sequence: previous,
                max_events: 100,
                wait_ms: 10000,
              });
              cursors.set(key, nextCursor({ events: batch.events ?? [], next_sequence: batch.next_sequence ?? previous }, previous));
              if (batch.events?.length) await refreshRef.current();
            }),
          );
          // A refresh also repairs gaps when an ephemeral topic is absent or has expired.
          if (liveRef.current && !signal.aborted) {
            await refreshRef.current();
            setStatus(`Live · updated ${new Date().toLocaleTimeString()}`);
          }
        } catch (error) {
          if (signal.aborted) break;
          setStatus(`Reconnecting · ${describe(error)}`);
        }
        await pause(Math.max(1000, 10000 - (Date.now() - began)), signal);
      }
    })();
    return () => {
      controller.abort();
    };
  }, [client]);
  return status;
}

/** Coalesced refreshes: one at a time, and one more when asked meanwhile (experiments.js refresh). */
export function useCoalesced(work: () => Promise<void>, onError: (error: unknown) => void): () => Promise<void> {
  const pending = useRef(false);
  const again = useRef(false);
  const workRef = useRef(work);
  workRef.current = work;
  const errorRef = useRef(onError);
  errorRef.current = onError;
  // One stable function for the page's life; it always runs the latest `work`.
  const run = useCallback(async (): Promise<void> => {
    if (pending.current) {
      again.current = true;
      return;
    }
    pending.current = true;
    try {
      await workRef.current();
    } catch (error) {
      errorRef.current(error);
    } finally {
      pending.current = false;
      if (again.current) {
        again.current = false;
        void run();
      }
    }
  }, []);
  return run;
}

/* ------------------------------------------------------------------ metrics */

/** The plot toolbar, a chart per metric grouped by name prefix, parameter differences and
 * artifacts (experiments.js plot_controls, drawMetrics and drawArtifacts). */
export function MetricsView({ details }: { details: RunDetail[] }) {
  const registry = useMemo(newRegistry, []);
  const [axis, setAxis] = useState("step");
  const [smoothing, setSmoothing] = useState(0);
  const [log, setLog] = useState(false);
  const weight = smoothing / 100;
  const names = useMemo(() => [...new Set(details.filter((run) => !run.metrics_suppressed).flatMap((run) => (run.series ?? []).map((series) => series.name)))].sort(), [details]);
  const labels = useMemo(() => details.map((run) => run.run), [details]);
  const groups = useMemo(() => {
    const out = new Map<string, string[]>();
    for (const name of names) {
      const prefix = name.includes(".") ? name.split(".")[0] : "Metrics";
      out.set(prefix, [...(out.get(prefix) ?? []), name]);
    }
    return out;
  }, [names]);
  const differences = useMemo(() => parameterDiff(details), [details]);
  return (
    <ChartContext.Provider value={registry}>
      <div className="experiment-toolbar">
        <label>
          X axis
          <select
            value={axis}
            onChange={(e) => {
              registry.views.clear();
              setAxis(e.target.value);
            }}
          >
            <option value="step">Step</option>
            <option value="relative">Relative time</option>
            <option value="wall">Wall time</option>
          </select>
        </label>
        <label>
          Smoothing (EMA) <output>{Math.round(weight * 100)}%</output>
          <input type="range" min={0} max={99} value={smoothing} onChange={(e) => setSmoothing(Number(e.target.value))} />
        </label>
        <label>
          <input type="checkbox" checked={log} onChange={(e) => setLog(e.target.checked)} /> Log scale
        </label>
        {(
          [
            ["in", "Zoom in"],
            ["out", "Zoom out"],
            ["left", "Pan left"],
            ["right", "Pan right"],
            ["reset", "Reset view"],
          ] as const
        ).map(([action, label]) => (
          <button key={action} type="button" onClick={() => viewAction(registry, action)}>
            {label}
          </button>
        ))}
      </div>
      <p className="sub">Drag a region to zoom; Shift-drag to pan. Hover for the crosshair and values. Non-positive values are omitted on a log axis and remain in the data table.</p>
      <div>
        {details
          .filter((run) => run.metrics_suppressed)
          .map((run) => (
            <p key={run.run}>{run.run}: curves suppressed for differential privacy</p>
          ))}
        {!names.length && <p>No training curves have been recorded. Live updates will appear here.</p>}
        {[...groups].map(([prefix, metrics]) => (
          <section key={prefix} className="panel">
            <h2>{prefix === "Metrics" ? prefix : `${prefix[0].toUpperCase()}${prefix.slice(1)} Metrics`}</h2>
            {metrics.map((name) => (
              <MetricPlot key={name} name={name} details={details} labels={labels} axis={axis} weight={weight} log={log} />
            ))}
          </section>
        ))}
      </div>
      <section className="panel">
        <h2>Parameter Differences</h2>
        <Table headers={["Parameter", "Differs", ...labels]} rows={differences.map((row) => [row.name, row.differs ? "Yes" : "No", ...row.values])} />
      </section>
      <section className="panel">
        <h2>Artifacts</h2>
        <Artifacts details={details} />
      </section>
    </ChartContext.Provider>
  );
}

function MetricPlot({ name, details, labels, axis, weight, log }: { name: string; details: RunDetail[]; labels: string[]; axis: string; weight: number; log: boolean }) {
  const data = useMemo(() => metricData(details, name, axis, weight, log), [details, name, axis, weight, log]);
  const rows = useMemo(
    () =>
      details.flatMap((run) => {
        if (run.metrics_suppressed) return [];
        const points = (run.series ?? []).find((series) => series.name === name)?.points ?? [];
        const smoothed = ema([...points].sort((a, b) => a.step - b.step), weight);
        const original = new Map(points.map((point) => [point.step, point.value]));
        return smoothed.map((point) => [run.run, point.step, point.time, original.get(point.step), point.value]);
      }),
    [details, name, weight],
  );
  return (
    <article className="experiment-plot">
      <h3>{name}</h3>
      <Chart
        chartKey={`metric:${name}`}
        data={data}
        labels={labels}
        settings={{ title: name, time: axis === "wall", log, metric: true, xLabel: axis === "step" ? "Step" : axis === "wall" ? "Wall time" : "Seconds since run start" }}
      />
      <details>
        <summary>Chart Data</summary>
        <Table headers={["Run", "Step", "Wall time", "Raw value", "Smoothed value"]} rows={rows} caption={name} />
      </details>
    </article>
  );
}

type Data = Record<string, unknown[]> & { labels?: unknown[]; matrix?: unknown[][]; rows?: Row[] };

function Artifacts({ details }: { details: RunDetail[] }) {
  const items = details.flatMap((run) => (run.artifacts ?? []).map((artifact) => ({ run: run.run, artifact })));
  if (!items.length) return <p>No artifacts have been recorded.</p>;
  return (
    <>
      {items.map(({ run, artifact }, index) => (
        <ArtifactView key={index} chartKey={`artifact:${index}`} run={run} name={artifact.name} type={artifact.type} data={(artifact.data ?? {}) as Data} />
      ))}
    </>
  );
}

function ArtifactView({ chartKey, run, name, type, data }: { chartKey: string; run: string; name: string; type: string; data: Data }) {
  const list = (key: string) => (Array.isArray(data[key]) ? data[key] : []) as unknown[];
  const numbers = (key: string) => list(key) as (number | null)[];
  const chart = useMemo(() => {
    if (type === "feature_importance") return { data: [numbers("values").map((_, i) => i), numbers("values")], labels: ["Importance"], settings: { xValues: list("features").map(String) } };
    if (type === "calibration") return { data: [numbers("predicted"), numbers("observed"), numbers("predicted")], labels: ["Observed", "Ideal"], settings: {} };
    return { data: [numbers("x"), numbers("y")], labels: [type === "roc_curve" ? "True positive rate" : "Precision"], settings: {} };
  }, [data, type]);
  let body: ReactNode;
  if (type === "confusion_matrix") {
    const labels = list("labels");
    const matrix = (Array.isArray(data.matrix) ? data.matrix : []) as unknown[][];
    body = <Table headers={["Actual / Predicted", ...labels.map(cell)]} rows={matrix.map((row, i) => [labels[i], ...row])} caption="Confusion Matrix" />;
  } else if (type === "sample_predictions") {
    const rows = (Array.isArray(data.rows) ? data.rows : []) as Row[];
    const keys = [...new Set(rows.flatMap((row) => Object.keys(row)))];
    body = <Table headers={keys} rows={rows.map((row) => keys.map((key) => row[key]))} caption="Sample Predictions" />;
  } else {
    let table: ReactNode;
    if (type === "feature_importance") {
      table = <Table headers={["Feature", "Importance"]} rows={list("features").map((feature, i) => [feature, list("values")[i]])} />;
    } else if (type === "calibration") {
      table = <Table headers={["Predicted", "Observed", "Count"]} rows={list("predicted").map((value, i) => [value, list("observed")[i], list("counts")[i]])} />;
    } else {
      table = <Table headers={["X", "Y", "Threshold"]} rows={list("x").map((value, i) => [value, list("y")[i], list("thresholds")[i]])} />;
    }
    body = (
      <>
        <Chart chartKey={chartKey} data={chart.data} labels={chart.labels} settings={chart.settings} />
        {table}
      </>
    );
  }
  return (
    <section className="experiment-artifact">
      <h3>
        {name} · {run}
      </h3>
      {body}
    </section>
  );
}
