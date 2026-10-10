/** Train → Experiments → one study (web_ui_experiments.cpp study mode; experiments.js drawStudy,
 * drawLeaderboard and refresh): status, Stop, optimization history, parallel coordinates,
 * parameter importance, the sortable trial leaderboard with Promote and Stop trial, and the latest
 * eight trials' curves, all refreshed live. */
import { useEffect, useMemo, useRef, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { type Row } from "@/lib/api";
import { Badge } from "@/lib/ui";
import { useSpacePermissions } from "@/pages/apps/shared";
import { Chart, ChartContext, newRegistry } from "./Chart";
import { describe, ExperimentsShell, MetricsView, Table, useCoalesced, useExperimentActions, useExperimentClient, useExperimentUrl, useFollow } from "./common";
import { cell, metricLabel, type RunDetail } from "./transforms";

type Trial = { number: number; run: string; state: string; params: Record<string, unknown>; objective?: number; cost?: number; model?: string } & Row;
type Study = { id: string; spec: Row; status: string; trials: Trial[]; spent_cost?: number; stop_reason?: string; promoted_model?: string } & Row;
type Analysis = { optimization_history?: Row[]; parameter_importances?: Row[]; parallel_coordinates?: Row[]; leaderboard?: Row[] };

export default function StudyPage() {
  const [params] = useSearchParams();
  const studyId = params.get("study") ?? "";
  const client = useExperimentClient();
  const url = useExperimentUrl();
  const navigate = useNavigate();
  const { canWrite, canAdmin: canPromote } = useSpacePermissions(client.customerId);
  const [notice, setNotice] = useState(params.get("error") ?? "");
  const [live, setLive] = useState(true);
  const [filter, setFilter] = useState("");
  const [study, setStudy] = useState<Study | null>(null);
  const [analysis, setAnalysis] = useState<Analysis>({});
  const [trials, setTrials] = useState<Trial[]>([]);
  const [trialMetrics, setTrialMetrics] = useState<RunDetail[]>([]);
  const [loaded, setLoaded] = useState(false);
  const cache = useRef({ analysis: {} as Analysis, trials: [] as Trial[], metrics: [] as RunDetail[], states: new Map<string, string>() });
  const filterRef = useRef(filter);
  filterRef.current = filter;

  const refresh = useCoalesced(
    async () => {
      // The study is required; an analysis or trial listing that cannot be read keeps the last one,
      // so one failed read never leaves the page without its charts (lake#449).
      const [data, nextAnalysis, listed] = await Promise.all([
        client.read<Study>("get_experiment_study", { study: studyId }),
        client.read<Analysis>("get_experiment_analysis", { study: studyId }).catch((error) => {
          setNotice(describe(error));
          return cache.current.analysis;
        }),
        client.read<{ trials?: Trial[] }>("list_experiment_trials", { study: studyId, ...(filterRef.current ? { state: filterRef.current } : {}) }).catch((error) => {
          setNotice(describe(error));
          return { trials: cache.current.trials };
        }),
      ]);
      const queue = [...(data.trials ?? [])];
      const refreshed = new Map<string, RunDetail>();
      await Promise.all(
        Array.from({ length: Math.min(4, queue.length) }, async () => {
          while (queue.length) {
            const trial = queue.shift()!;
            const cached = cache.current.metrics.find((run) => run.run === trial.run);
            const unchanged = trial.state !== "running" && cache.current.states.get(trial.run) === trial.state;
            try {
              refreshed.set(trial.run, unchanged && cached ? cached : await client.read<RunDetail>("get_run_metrics", { run_id: trial.run }));
              cache.current.states.set(trial.run, trial.state);
            } catch {
              // A trial whose curves cannot be read keeps its last curves, or has none.
              if (cached) refreshed.set(trial.run, cached);
            }
          }
        }),
      );
      const metrics = (data.trials ?? []).map((trial) => refreshed.get(trial.run)).filter((run): run is RunDetail => !!run);
      cache.current = { ...cache.current, analysis: nextAnalysis ?? {}, trials: listed.trials ?? [], metrics };
      setStudy(data);
      setAnalysis(nextAnalysis ?? {});
      setTrials(listed.trials ?? []);
      setTrialMetrics(metrics);
      setLoaded(true);
    },
    (error) => {
      setNotice(describe(error));
      setLoaded(true);
    },
  );
  useEffect(() => {
    void refresh();
  }, [refresh, studyId, filter]);
  const details = useMemo(() => trialMetrics.slice(-8), [trialMetrics]);
  const status = useFollow(
    live,
    () => [{ kind: "study" as const, id: studyId }, ...details.filter((run) => !run.metrics_suppressed).map((run) => ({ kind: "run" as const, id: run.run }))],
    refresh,
  );
  const actions = useExperimentActions(refresh, setNotice);

  if (!study) {
    return (
      <ExperimentsShell mode="study" notice={notice} live={live} onLive={setLive} status={status}>
        {loaded ? null : <p className="muted">Loading the study…</p>}
      </ExperimentsShell>
    );
  }
  const spec = study.spec ?? {};
  const objective = (spec.objective ?? {}) as Row;
  const budgets = (spec.budgets ?? {}) as Row;
  const statusLine =
    `State: ${study.status} · Trials: ${study.trials.length} / ${cell(budgets.max_trials)} · Cost: ${cell(study.spent_cost)} / ${cell(budgets.cost_cap)}` +
    ` · Objective: ${metricLabel(objective.metric)} (${cell(objective.direction)})` +
    (study.stop_reason ? ` · ${study.stop_reason}` : "") +
    (study.promoted_model ? ` · Promoted model: ${study.promoted_model}` : "");

  return (
    <ExperimentsShell mode="study" notice={notice} live={live} onLive={setLive} status={status}>
      {actions.dialog}
      <section className="panel">
        <h2>{cell(spec.name)}</h2>
        <p>{statusLine}</p>
        {canWrite && study.status === "running" && (
          <button type="button" disabled={actions.busy !== null} onClick={() => void actions.perform("stop_study", studyId)}>
            Stop study
          </button>
        )}
        {study.promoted_model && (
          <p>
            <Badge tone="success">promoted</Badge> Model <code>{study.promoted_model}</code> is this study's promoted model.
          </p>
        )}
      </section>
      <StudyCharts analysis={analysis} />
      <section className="panel">
        <h2>Trial Leaderboard</h2>
        <label>
          Trial state
          <select value={filter} onChange={(e) => setFilter(e.target.value)}>
            <option value="">All states</option>
            <option>running</option>
            <option>complete</option>
            <option>pruned</option>
            <option>failed</option>
          </select>
        </label>
        <Leaderboard
          study={study}
          trials={trials}
          analysis={analysis}
          trialMetrics={trialMetrics}
          canWrite={canWrite}
          canPromote={canPromote}
          busy={actions.busy}
          perform={(capability, run) => void actions.perform(capability, studyId, run)}
          onCompare={(runs) => {
            if (runs.length < 2 || runs.length > 8) {
              setNotice("Select two to eight runs to compare.");
              return;
            }
            navigate(url("/train/experiments/compare", { runs: runs.join(",") }));
          }}
          runHref={(run) => url("/train/experiment/run", { run })}
        />
      </section>
      <h2>Latest Trial Curves</h2>
      <p>The latest eight trial runs appear below. Open any trial for its full charts and artifacts.</p>
      <MetricsView details={details} />
    </ExperimentsShell>
  );
}

function StudyCharts({ analysis }: { analysis: Analysis }) {
  const registry = useMemo(newRegistry, []);
  const history = analysis.optimization_history ?? [];
  const historyData = useMemo(() => [history.map((row) => row.number), history.map((row) => row.value), history.map((row) => row.best)], [history]);
  const rows = analysis.parallel_coordinates ?? [];
  const parameters = useMemo(() => [...new Set(rows.flatMap((row) => Object.keys(row.params ?? {})))].sort(), [rows]);
  const labels = useMemo(() => [...parameters, "Objective"], [parameters]);
  const raw = useMemo(() => rows.map((row) => [...parameters.map((key) => (row.params ?? {})[key]), row.value]), [rows, parameters]);
  const parallelData = useMemo(() => {
    // Each axis normalized from 0 to 1; categorical values by first occurrence.
    const normalized = labels.map((_, index) => {
      const values = raw.map((row) => row[index]);
      const numeric = values.every((value) => value === undefined || typeof value === "number");
      const categories = [...new Set(values.filter((value) => value !== undefined))];
      const numbers = numeric ? (values as (number | undefined)[]) : values.map((value) => categories.indexOf(value));
      const finite = numbers.filter((n): n is number => Number.isFinite(n));
      const min = finite.length ? Math.min(...finite) : 0;
      const max = finite.length ? Math.max(...finite) : 1;
      return numbers.map((value, row) => (values[row] === undefined ? null : max === min ? 0.5 : ((value as number) - min) / (max - min)));
    });
    return [labels.map((_, index) => index), ...rows.map((_, row) => normalized.map((column) => column[row]))];
  }, [labels, raw, rows]);
  const runLabels = useMemo(() => rows.map((row) => String(row.run ?? "")), [rows]);
  const importance = analysis.parameter_importances ?? [];
  const importanceData = useMemo(() => [importance.map((_, index) => index), importance.map((row) => row.importance)], [importance]);
  const importanceNames = useMemo(() => importance.map((row) => String(row.parameter ?? "")), [importance]);
  return (
    <ChartContext.Provider value={registry}>
      <section className="panel">
        <h2>Optimization History</h2>
        <Chart chartKey="history" data={historyData} labels={HISTORY_LABELS} settings={{ title: "Optimization History" }} />
        {!history.length && <p>No trial has completed yet.</p>}
        <Table headers={["Trial", "Run", "Objective", "Best so far"]} rows={history.map((row) => [row.number, row.run, row.value, row.best])} />
      </section>
      <section className="panel">
        <h2>Parallel Coordinates</h2>
        <Chart chartKey="parallel" data={parallelData} labels={runLabels} settings={{ xValues: labels, title: "Parallel Coordinates" }} />
        <p>Axes are normalized from 0 to 1; categorical values follow first occurrence. Exact values:</p>
        <Table headers={["Run", ...labels]} rows={rows.map((row, index) => [row.run, ...raw[index]])} />
      </section>
      <section className="panel">
        <h2>Parameter Importance</h2>
        <Chart chartKey="importance" data={importanceData} labels={IMPORTANCE_LABELS} settings={{ xValues: importanceNames, title: "Parameter Importance" }} />
        {!importance.length && <p>Parameter importance needs completed trials.</p>}
        <Table headers={["Parameter", "Importance"]} rows={importance.map((row) => [row.parameter, row.importance])} />
      </section>
    </ChartContext.Provider>
  );
}

const HISTORY_LABELS = ["Objective", "Best so far"];
const IMPORTANCE_LABELS = ["Importance"];

function Leaderboard({
  study,
  trials,
  analysis,
  trialMetrics,
  canWrite,
  canPromote,
  busy,
  perform,
  onCompare,
  runHref,
}: {
  study: Study;
  trials: Trial[];
  analysis: Analysis;
  trialMetrics: RunDetail[];
  canWrite: boolean;
  canPromote: boolean;
  busy: string | null;
  perform: (capability: "stop_experiment_trial" | "promote_trial", run: string) => void;
  onCompare: (runs: string[]) => void;
  runHref: (run: string) => string;
}) {
  const [hidden, setHidden] = useState<Set<string>>(new Set());
  const [sortKey, setSortKey] = useState("number");
  const [descending, setDescending] = useState(false);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const available = useMemo(
    () => [
      "number",
      "state",
      "objective",
      "cost",
      "confidence",
      ...new Set(study.trials.flatMap((trial) => Object.keys(trial.params ?? {}).map((key) => `param:${key}`))),
      ...new Set(trialMetrics.flatMap((run) => Object.keys(run.summary ?? {}).map((key) => `metric:${key}`))),
    ],
    [study.trials, trialMetrics],
  );
  const visible = available.filter((key) => !hidden.has(key));
  const value = (trial: Trial, key: string): unknown => {
    if (key.startsWith("param:")) return trial.params?.[key.slice(6)];
    if (key.startsWith("metric:")) return trialMetrics.find((run) => run.run === trial.run)?.summary?.[key.slice(7)]?.final;
    if (key === "confidence") {
      const row = analysis.leaderboard?.find((entry) => entry.run === trial.run);
      return row ? `${row.confidence_low} – ${row.confidence_high}` : undefined;
    }
    return trial[key];
  };
  const sorted = [...trials].sort((a, b) => {
    const x = value(a, sortKey);
    const y = value(b, sortKey);
    const order = typeof x === "number" && typeof y === "number" ? x - y : String(x ?? "").localeCompare(String(y ?? ""));
    return descending ? -order : order;
  });
  const label = (key: string) => key.replace(/^(param|metric):/, "");
  return (
    <>
      <details>
        <summary>Columns</summary>
        <div className="trial-columns">
          {available.map((key) => (
            <label key={key}>
              <input
                type="checkbox"
                checked={!hidden.has(key)}
                onChange={(e) =>
                  setHidden((current) => {
                    const next = new Set(current);
                    if (e.target.checked) next.delete(key);
                    else next.add(key);
                    return next;
                  })
                }
              />
              {label(key)}
            </label>
          ))}
        </div>
      </details>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          onCompare([...selected]);
        }}
      >
        <div className="table-scroll">
          <table>
            <thead>
              <tr>
                <th>Compare</th>
                <th>Run</th>
                {visible.map((key) => (
                  <th key={key} scope="col" aria-sort={sortKey === key ? (descending ? "descending" : "ascending") : "none"}>
                    <button
                      type="button"
                      className="th-sort"
                      onClick={() => {
                        setDescending(sortKey === key ? !descending : false);
                        setSortKey(key);
                      }}
                    >
                      {label(key)}
                    </button>
                  </th>
                ))}
                <th>Actions</th>
              </tr>
            </thead>
            <tbody>
              {sorted.map((trial) => {
                const capability = trial.state === "running" && canWrite ? "stop_experiment_trial" : trial.state === "complete" && trial.model && canPromote ? "promote_trial" : null;
                return (
                  <tr key={trial.run}>
                    <td>
                      <input
                        type="checkbox"
                        aria-label={`Compare trial ${trial.number}`}
                        checked={selected.has(trial.run)}
                        onChange={(e) =>
                          setSelected((current) => {
                            const next = new Set(current);
                            if (e.target.checked) next.add(trial.run);
                            else next.delete(trial.run);
                            return next;
                          })
                        }
                      />
                    </td>
                    <td>
                      <Link to={runHref(trial.run)}>{trial.run}</Link>
                      {study.promoted_model && trial.model === study.promoted_model ? (
                        <>
                          {" "}
                          <Badge tone="success">promoted</Badge>
                        </>
                      ) : null}
                    </td>
                    {visible.map((key) => (
                      <td key={key}>{cell(value(trial, key))}</td>
                    ))}
                    <td>
                      {capability && (
                        <button type="button" disabled={busy !== null} onClick={() => perform(capability, trial.run)}>
                          {capability === "promote_trial" ? "Promote" : "Stop trial"}
                        </button>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        <button type="submit">Compare selected runs</button>
      </form>
    </>
  );
}
