// SPDX-License-Identifier: MIT

/** Train → Experiments → one run's charts (web_ui_experiments.cpp, run mode): its curves,
 * parameters and artifacts, refreshed live from `tail_experiment_events`. */
import { useEffect, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { describe, ExperimentsShell, MetricsView, useCoalesced, useExperimentClient, useFollow } from "./common";
import type { RunDetail } from "./transforms";

export default function RunPage() {
  const [params] = useSearchParams();
  const run = params.get("run") ?? "";
  const client = useExperimentClient();
  const [details, setDetails] = useState<RunDetail[]>([]);
  const [notice, setNotice] = useState(params.get("error") ?? "");
  const [live, setLive] = useState(true);
  const refresh = useCoalesced(
    async () => {
      setDetails([await client.read<RunDetail>("get_run_metrics", { run_id: run })]);
    },
    (error) => setNotice(describe(error)),
  );
  useEffect(() => {
    void refresh();
  }, [refresh, run]);
  const status = useFollow(live, () => details.filter((detail) => !detail.metrics_suppressed).map((detail) => ({ kind: "run" as const, id: detail.run })), refresh);
  return (
    <ExperimentsShell
      mode="run"
      notice={notice}
      live={live}
      onLive={setLive}
      status={status}
      sub={
        <>
          Curves, parameters and artifacts of run <code>{run}</code>.
        </>
      }
    >
      <MetricsView details={details} />
    </ExperimentsShell>
  );
}
