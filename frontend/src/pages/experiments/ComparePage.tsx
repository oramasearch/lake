/** Train → Experiments → Compare Runs (web_ui_experiments.cpp, compare mode): two to eight runs'
 * curves on shared charts, their parameter differences and artifacts, refreshed live — the runs
 * compared on top, then one clean chart per metric with its table. */
import { useEffect, useMemo, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { GitCompare } from "lucide-react";
import { Button, Card, Chip, Empty, Stat, StatRow } from "@/lib/ui";
import { describe, ExperimentsShell, MetricsView, useCoalesced, useExperimentClient, useExperimentUrl, useFollow } from "./common";
import { canCompare, chosenRuns, MAX_COMPARED, type RunDetail } from "./transforms";

export default function ComparePage() {
  const [params] = useSearchParams();
  const runsParam = params.get("runs") ?? "";
  const runs = useMemo(() => chosenRuns(runsParam), [runsParam]);
  // With fewer than two runs (or more than eight) nothing is asked of lake (lake#576).
  const comparable = canCompare(runs);
  const client = useExperimentClient();
  const url = useExperimentUrl();
  const [details, setDetails] = useState<RunDetail[]>([]);
  const [notice, setNotice] = useState(params.get("error") ?? "");
  const [live, setLive] = useState(true);
  const refresh = useCoalesced(
    async () => {
      const value = await client.read<{ details?: RunDetail[] }>("compare_runs", { runs });
      setDetails(value.details ?? []);
    },
    (error) => setNotice(describe(error)),
  );
  useEffect(() => {
    if (comparable) void refresh();
  }, [refresh, runs, comparable]);
  const status = useFollow(live && comparable, () => details.filter((detail) => !detail.metrics_suppressed).map((detail) => ({ kind: "run" as const, id: detail.run })), refresh);
  const metrics = new Set(details.filter((run) => !run.metrics_suppressed).flatMap((run) => (run.series ?? []).map((series) => series.name)).filter((name) => !name.startsWith("held_out_"))).size;
  const artifacts = details.reduce((sum, run) => sum + (run.artifacts ?? []).length, 0);
  const sub = "Two to eight runs' held-out scores and curves on shared charts, with their parameter differences and artifacts.";
  if (!comparable) {
    return (
      <ExperimentsShell mode="compare" notice={notice} live={false} onLive={setLive} status="" sub={sub}>
        <Empty
          icon={GitCompare}
          title={runs.length > MAX_COMPARED ? "Choose at Most Eight Runs to Compare" : "Choose Two or More Runs to Compare"}
          action={<Button to={url("/train/experiments")}>Choose runs</Button>}
        >
          {runs.length === 1
            ? "Only one run is chosen. Select at least one more in the tracked runs, then press Compare."
            : runs.length > MAX_COMPARED
              ? `${runs.length} runs are chosen. Compare Runs shows two to eight at a time.`
              : "Select two to eight runs in the tracked runs, then press Compare."}
        </Empty>
      </ExperimentsShell>
    );
  }
  return (
    <ExperimentsShell mode="compare" notice={notice} live={live} onLive={setLive} status={status} sub={sub}>
      <StatRow cols={3}>
        <Stat label="Runs compared" value={runs.length} icon={GitCompare} />
        <Stat label="Metrics" value={metrics} sub="curves recorded" />
        <Stat label="Artifacts" value={artifacts} />
      </StatRow>
      <Card label="Runs" className="mb-4">
        <div className="flex flex-wrap gap-1.5">
          {runs.map((run) => (
            <Link key={run} to={url("/train/experiment/run", { run })} className="no-underline">
              <Chip>
                <span className="font-mono">{run}</span>
              </Chip>
            </Link>
          ))}
        </div>
      </Card>
      <MetricsView details={details} />
    </ExperimentsShell>
  );
}
