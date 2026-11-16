// SPDX-License-Identifier: MIT

/** Train → Runs (web_ui.cpp workspace_content "runs"): every pipeline run with figures, a search
 * and status filters, its status and its report. Also the run and experiment helpers the Run and
 * Models pages share (experimentRefs, RunSubject, TrialSubject, when). */
import { useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { CheckCircle2, Loader, Play, XCircle } from "lucide-react";
import type { Row } from "@/lib/api";
import { titleCase } from "@/lib/format";
import { useTrail } from "@/lib/trail";
import { Button, DataTable, Empty, FilterChips, Mono, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, text, timeAgo, useWorkspace, useWq, useWsPath } from "./workspace";

/* ------------------------------------------------------------------ shared with Run and Models */

/** "2026-09-27T13:29:22.572Z" → "2026-09-27 13:29 UTC", as lake printed a run's start. */
export function when(timestamp: string): string {
  if (timestamp.length < 16) return timestamp;
  return `${timestamp.slice(0, 10)} ${timestamp.slice(11, 16)} UTC`;
}

export type ExperimentRef = { id: string; name: string; number?: unknown };
export type ExperimentRefs = { byRun: Map<string, ExperimentRef>; byModel: Map<string, ExperimentRef>; promoted: Set<string> };

/** Which experiment each trial run and trial model belongs to, and which models an experiment
 * promoted (lake#449), from the engine's `list_studies` (an experiment is the engine's study). */
export function experimentRefs(listing: Row | undefined): ExperimentRefs {
  const refs: ExperimentRefs = { byRun: new Map(), byModel: new Map(), promoted: new Set() };
  for (const experiment of (listing?.studies ?? []) as Row[]) {
    if (!experiment || typeof experiment !== "object") continue;
    const spec = (experiment.spec ?? {}) as Row;
    const ref: ExperimentRef = { id: text(experiment, "id"), name: text(experiment, "name") || text(spec, "name") };
    for (const trial of (experiment.trials ?? []) as Row[]) {
      if (!trial || typeof trial !== "object") continue;
      const entry = { ...ref, number: trial.number };
      if (text(trial, "run")) refs.byRun.set(text(trial, "run"), entry);
      if (text(trial, "model")) refs.byModel.set(text(trial, "model"), entry);
    }
    const promoted = text(experiment, "promoted_model");
    if (promoted) {
      refs.promoted.add(promoted);
      if (!refs.byModel.has(promoted)) refs.byModel.set(promoted, ref);
    }
  }
  return refs;
}

/** The workspace's experiments; a failure is not shown (the pages read them only to name trials). */
export function useExperimentRefs() {
  const q = useWq<Row>("list_studies");
  return experimentRefs(q.data);
}

/** "Trial N of experiment <name>" (or "A trial of experiment <name>"), the experiment linked. */
export function TrialSubject({ refOf }: { refOf: ExperimentRef }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  return (
    <>
      {typeof refOf.number === "number" ? `Trial ${refOf.number} of ` : "A trial of "}experiment{" "}
      <Link to={path(`/train/experiment?${qs({ customer_id: customerId, experiment: refOf.id })}`)}>{refOf.name || refOf.id}</Link>
    </>
  );
}

/** A run's pipeline and project as links (web_ui.cpp run_subject). Runs carry `pipeline_name` and
 * `project_name`; older ones only the graph and project ids, which are shown as they are. */
export function RunSubject({ run }: { run: Row }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const pipelineName = text(run, "pipeline_name");
  const name = pipelineName || text(run, "graph");
  const project = pipelineName ? text(run, "project_name") : text(run, "project");
  if (!name && !project) return <span className="muted">a run outside any pipeline</span>;
  if (!project) return <>{name}</>;
  const projectLink = <Link to={path(`/design/project?${qs({ customer_id: customerId, name: project })}`)}>{project}</Link>;
  if (!name)
    return (
      <>
        <span className="muted">a run</span> <span className="muted">in</span> {projectLink}
      </>
    );
  return (
    <>
      <Link to={path(`/design/pipeline?${qs({ customer_id: customerId, project, graph: name })}`)}>{name}</Link> <span className="muted">in</span>{" "}
      {projectLink}
    </>
  );
}

export { ObjectTable } from "./datasets";

export function runStatusTone(status: string) {
  return status === "succeeded" ? "success" : status === "failed" ? "danger" : UNFINISHED.has(status) ? "warning" : "neutral";
}

/* ------------------------------------------------------------------ the page */

/** A run that has not finished (the Run page asks for the report only after). */
export const UNFINISHED = new Set(["running", "queued", "pending"]);

export default function RunsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Train", to: path(`/train/agents?${space}`) },
    { label: "Runs", to: path(`/train/runs?${space}`) },
  ]);
  return (
    <WorkspaceFrame eyebrow="Train" title="Runs" sub="Every pipeline run: what it trained, its status, and its report.">
      {() => <Runs />}
    </WorkspaceFrame>
  );
}

function Runs() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const q = useWq<Row>("list_runs");
  const experiments = useExperimentRefs();
  const [search, setSearch] = useState("");
  const [status, setStatus] = useState<string | null>(null);
  const runs = useMemo(() => ((q.data?.runs ?? []) as Row[]).filter((run) => run && typeof run === "object"), [q.data]);
  const statuses = useMemo(() => {
    const counts = new Map<string, number>();
    for (const run of runs) {
      const s = text(run, "status") || "unknown";
      counts.set(s, (counts.get(s) ?? 0) + 1);
    }
    return [...counts.entries()].map(([id, count]) => ({ id, label: titleCase(id), count }));
  }, [runs]);
  const count = (s: string) => runs.filter((run) => text(run, "status") === s).length;
  const unfinished = runs.filter((run) => UNFINISHED.has(text(run, "status"))).length;
  const needle = search.trim().toLowerCase();
  const shown = runs.filter((run) => {
    if (status && (text(run, "status") || "unknown") !== status) return false;
    if (!needle) return true;
    const trial = experiments.byRun.get(text(run, "id"));
    const words = [text(run, "pipeline_name"), text(run, "project_name"), text(run, "graph"), text(run, "project"), text(run, "id"), trial?.name ?? ""].join(" ");
    return words.toLowerCase().includes(needle);
  });
  const projects = path(`/design/projects?${qs({ customer_id: customerId })}`);
  return (
    <>
      {q.data && runs.length > 0 && (
        <StatRow>
          <Stat label="Runs" icon={Play} value={runs.length} sub="in this workspace" />
          <Stat label="Succeeded" icon={CheckCircle2} value={count("succeeded")} tone={count("succeeded") ? "ok" : undefined} />
          <Stat label="Failed" icon={XCircle} value={count("failed")} tone={count("failed") ? "bad" : undefined} sub={count("failed") ? "open one to see why" : "none"} />
          <Stat label="In progress" icon={Loader} value={unfinished} sub="running, queued or pending" />
        </StatRow>
      )}
      {q.data && runs.length > 0 && (
        <Toolbar>
          <SearchInput value={search} onChange={setSearch} placeholder="Search runs…" />
          <FilterChips value={status} onChange={setStatus} options={statuses} allLabel="All statuses" />
        </Toolbar>
      )}
      <DataTable<Row>
        q={q}
        what="runs"
        rows={shown}
        rowKey={(run, i) => text(run, "id") || String(i)}
        initialSort={{ key: "created", dir: "descending" }}
        empty={
          runs.length === 0 ? (
            <Empty icon={Play} title="No runs yet" action={<Button to={projects}>Open projects</Button>}>
              Run a pipeline from a project.
            </Empty>
          ) : (
            <Empty compact title="No run matches">
              Clear the search or pick another status.
            </Empty>
          )
        }
        columns={[
          {
            key: "pipeline",
            label: "Pipeline",
            sort: (run) => (text(run, "pipeline_name") || text(run, "graph")).toLowerCase(),
            render: (run) => {
              const trial = experiments.byRun.get(text(run, "id"));
              return (
                <div className="min-w-0">
                  <div className="font-semibold">{trial ? <TrialSubject refOf={trial} /> : <RunSubject run={run} />}</div>
                  <div className="text-[11.5px] text-ink-3">
                    <Mono short={8}>{text(run, "id")}</Mono>
                  </div>
                </div>
              );
            },
          },
          {
            key: "status",
            label: "Status",
            sort: (run) => text(run, "status"),
            render: (run) => <Status value={text(run, "status") || "unknown"} tone={runStatusTone(text(run, "status"))} />,
          },
          {
            key: "created",
            label: "Started",
            sort: (run) => text(run, "created"),
            render: (run) => (
              <span className="font-mono text-[12px] text-ink-2" title={when(text(run, "created"))}>
                {timeAgo(text(run, "created"))}
              </span>
            ),
          },
          {
            key: "links",
            label: "",
            className: "text-right",
            render: (run) => {
              const id = text(run, "id");
              return (
                <div className="flex flex-wrap justify-end gap-1.5">
                  <Button variant="ghost" to={path(`/train/run?${qs({ customer_id: customerId, id })}`)}>
                    {text(run, "status") === "failed" ? "Why it failed" : "Report"}
                  </Button>
                  <Button variant="ghost" to={path(`/train/experiment/run?${qs({ customer_id: customerId, run: id })}`)}>
                    Charts
                  </Button>
                </div>
              );
            },
          },
        ]}
      />
    </>
  );
}
