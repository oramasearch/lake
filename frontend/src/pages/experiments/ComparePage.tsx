/** Train → Experiments → Compare Runs (web_ui_experiments.cpp, compare mode): two to eight runs'
 * curves on shared charts, their parameter differences and artifacts, refreshed live. */
import { useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { describe, ExperimentsShell, MetricsView, useCoalesced, useExperimentClient, useFollow } from "./common";
import type { RunDetail } from "./transforms";

export default function ComparePage() {
  const [params] = useSearchParams();
  const runsParam = params.get("runs") ?? "";
  const runs = useMemo(() => runsParam.split(","), [runsParam]);
  const client = useExperimentClient();
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
  return (
    <ExperimentsShell mode="compare" notice={notice} live={live} onLive={setLive} status={status}>
      <MetricsView details={details} />
    </ExperimentsShell>
  );
}
