/** Train → Runs (web_ui.cpp workspace_content "runs"): every pipeline run, its status and its
 * report. Also the run and study helpers the Run and Models pages share (study_refs, run_subject,
 * trial_subject, when). */
import { Fragment } from "react";
import { Link } from "react-router-dom";
import type { Row } from "@/lib/api";
import { useTrail } from "@/lib/trail";
import { Badge, DataTable, Query } from "@/lib/ui";
import { WorkspaceFrame, qs, text, useWorkspace, useWq, useWsPath } from "./workspace";

/* ------------------------------------------------------------------ shared with Run and Models */

/** "2026-09-27T13:29:22.572Z" → "2026-09-27 13:29 UTC", as lake printed a run's start. */
export function when(timestamp: string): string {
  if (timestamp.length < 16) return timestamp;
  return `${timestamp.slice(0, 10)} ${timestamp.slice(11, 16)} UTC`;
}

export type StudyRef = { id: string; name: string; number?: unknown };
export type StudyRefs = { byRun: Map<string, StudyRef>; byModel: Map<string, StudyRef>; promoted: Set<string> };

/** Which study each trial run and trial model belongs to, and which models a study promoted
 * (lake#449), from `list_studies`. */
export function studyRefs(studies: Row | undefined): StudyRefs {
  const refs: StudyRefs = { byRun: new Map(), byModel: new Map(), promoted: new Set() };
  for (const study of (studies?.studies ?? []) as Row[]) {
    if (!study || typeof study !== "object") continue;
    const spec = (study.spec ?? {}) as Row;
    const ref: StudyRef = { id: text(study, "id"), name: text(study, "name") || text(spec, "name") };
    for (const trial of (study.trials ?? []) as Row[]) {
      if (!trial || typeof trial !== "object") continue;
      const entry = { ...ref, number: trial.number };
      if (text(trial, "run")) refs.byRun.set(text(trial, "run"), entry);
      if (text(trial, "model")) refs.byModel.set(text(trial, "model"), entry);
    }
    const promoted = text(study, "promoted_model");
    if (promoted) {
      refs.promoted.add(promoted);
      if (!refs.byModel.has(promoted)) refs.byModel.set(promoted, ref);
    }
  }
  return refs;
}

/** The space's studies; a failure is not shown (the pages read them only to name trials). */
export function useStudies() {
  const q = useWq<Row>("list_studies");
  return studyRefs(q.data);
}

/** "Trial N of study <name>" (or "A trial of study <name>"), the study linked. */
export function TrialSubject({ refOf }: { refOf: StudyRef }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  return (
    <>
      {typeof refOf.number === "number" ? `Trial ${refOf.number} of ` : "A trial of "}study{" "}
      <Link to={path(`/train/experiment?${qs({ customer_id: customerId, study: refOf.id })}`)}>{refOf.name || refOf.id}</Link>
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
  return status === "succeeded" ? "success" : status === "failed" ? "danger" : "neutral";
}

/* ------------------------------------------------------------------ the page */

export default function RunsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Train", to: path(`/train/data?${space}`) },
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
  const studies = useStudies();
  return (
    <Query
      q={q}
      what="runs"
      isEmpty={(v) => !((v.runs ?? []) as Row[]).length}
      empty={<div className="panel empty">No runs yet. Run a pipeline from a project.</div>}
    >
      {(value) => (
        <DataTable<Row>
          rows={(value.runs ?? []) as Row[]}
          what="runs"
          rowKey={(run, i) => text(run, "id") || String(i)}
          columns={[
            {
              key: "pipeline",
              label: "Pipeline",
              render: (run) => {
                const trial = studies.byRun.get(text(run, "id"));
                return trial ? <TrialSubject refOf={trial} /> : <RunSubject run={run} />;
              },
            },
            {
              key: "status",
              label: "Status",
              render: (run) => <Badge tone={runStatusTone(text(run, "status"))}>{text(run, "status")}</Badge>,
            },
            { key: "created", label: "Started", render: (run) => when(text(run, "created")) },
            {
              key: "links",
              label: "",
              render: (run) => {
                const id = text(run, "id");
                return (
                  <Fragment>
                    <Link to={path(`/train/run?${qs({ customer_id: customerId, id })}`)}>{text(run, "status") === "failed" ? "Why it failed" : "Report"}</Link>
                    {" · "}
                    <Link to={path(`/train/experiment/run?${qs({ customer_id: customerId, run: id })}`)}>Charts</Link>
                  </Fragment>
                );
              },
            },
          ]}
        />
      )}
    </Query>
  );
}

