/** Train → Experiments → Compare Runs (web_ui_experiments.cpp, compare mode): two to eight runs'
 * curves on shared charts, their parameter differences and artifacts, refreshed live — the runs
 * compared on top, then one clean chart per metric with its table. */
import { useEffect, useMemo, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { GitCompare } from "lucide-react";
import { Card, Chip, Stat, StatRow } from "@/lib/ui";
import { describe, ExperimentsShell, MetricsView, useCoalesced, useExperimentClient, useExperimentUrl, useFollow } from "./common";
import type { RunDetail } from "./transforms";

export default function ComparePage() {
  const [params] = useSearchParams();
  const runsParam = params.get("runs") ?? "";
  const runs = useMemo(() => runsParam.split(","), [runsParam]);
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
    void refresh();
  }, [refresh, runs]);
  const status = useFollow(live, () => details.filter((detail) => !detail.metrics_suppressed).map((detail) => ({ kind: "run" as const, id: detail.run })), refresh);
  const metrics = new Set(details.filter((run) => !run.metrics_suppressed).flatMap((run) => (run.series ?? []).map((series) => series.name))).size;
  const artifacts = details.reduce((sum, run) => sum + (run.artifacts ?? []).length, 0);
  return (
    <ExperimentsShell mode="compare" notice={notice} live={live} onLive={setLive} status={status} sub="Two to eight runs' curves on shared charts, with their parameter differences and artifacts.">
      <StatRow cols={3}>
        <Stat label="Runs compared" value={runs.filter(Boolean).length} icon={GitCompare} />
        <Stat label="Metrics" value={metrics} sub="curves recorded" />
        <Stat label="Artifacts" value={artifacts} />
      </StatRow>
      <Card label="Runs" className="mb-4">
        <div className="flex flex-wrap gap-1.5">
          {runs.filter(Boolean).map((run) => (
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
