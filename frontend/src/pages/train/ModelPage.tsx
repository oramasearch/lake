/** Train → Model (`/train/model?id=`, lake#599): training one model. Its versions (one per run or
 * trial) with their held-out scores over time, two versions compared, what the chosen version
 * learned from, the pipeline step or steps it belongs to, retraining (its pipeline run again) and
 * promoting a version to Ship. Serving it is Ship → Model, linked from the header.
 *
 * A standalone model (lake#654) shows what it trains on instead of a pipeline, and has Retrain and
 * New version (`retrain_model`, or a new file for a brought one), Move into a project
 * (`move_model_to_project`) and Delete (`delete_model` for each version).
 *
 * Any version's weights download as a file (`export_model`, lake#681): as Lake keeps them, or a
 * tree model as ONNX to score elsewhere. */
import { useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ArrowUpRight, Download, FolderInput, GitCompare, Plus, RefreshCw, Rocket, Trash2 } from "lucide-react";
import { act, ApiError, read, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, FormError, KeyValue, Mono, Skeleton, Split, Stat, StatRow, Status } from "@/lib/ui";
import { Chart } from "../experiments/Chart";
import { WorkspaceFrame, cellText, qs, text, useWorkspace, useWq, useWsPath, wa } from "./workspace";
import { when } from "./RunsPage";
import { broughtFrom, compareVersions, exportsAsOnnx, featuresOf, groupOf, measuredFigures, modelSteps, scoreSeries, trainingOf, versionNumber, weightsFile, type ModelGroup } from "./models";
import { ScoreText, SourceText, VersionScore, useModelGroups, useModelLinks } from "./modelParts";
import { BringDialog, RetrainDialog } from "./NewModel";

export default function TrainModelPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const links = useModelLinks();
  const space = qs({ customer_id: customerId });
  const { q, groups, experiments } = useModelGroups();
  const group = groupOf(groups, id);
  useTrail([
    { label: "Train", to: path(`/train/agents?${space}`) },
    { label: "Models", to: links.trainList },
    entityCrumb(group?.name ?? id),
  ]);
  const published = group?.published[0];
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title={
        group ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {group.name}
            {published ? <Status value={`v${versionNumber(group, text(published, "id"))} serving`} tone="success" /> : <Status value="not published" tone="neutral" />}
          </span>
        ) : (
          "Model"
        )
      }
      sub="Training this model: its versions, their held-out scores and what each learned from."
      actions={
        id ? (
          <Button to={links.ship(text(published, "id") || id)}>
            <ArrowUpRight size={14} aria-hidden /> Serving in Ship
          </Button>
        ) : null
      }
    >
      {() => {
        if (q.isLoading) return <Skeleton lines={6} />;
        if (q.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what="the models" />;
        if (!group)
          return (
            <Empty title="No such model" action={<Button to={links.trainList}>Every model</Button>}>
              This workspace has no model version <code>{id || "(none named)"}</code>.
            </Empty>
          );
        const experiment = group.versions.map((v) => experiments.byModel.get(text(v, "id")) ?? experiments.byRun.get(text(v, "run"))).find(Boolean);
        return <ModelBody key={id} group={group} groups={groups} id={id} experiment={experiment} promoted={experiments.promoted} />;
      }}
    </WorkspaceFrame>
  );
}

function ModelBody({ group, groups, id, experiment, promoted }: { group: ModelGroup; groups: ModelGroup[]; id: string; experiment?: { id: string; name: string }; promoted: Set<string> }) {
  const path = useWsPath();
  const scope = useWorkspace();
  const links = useModelLinks();
  const selected = group.versions.find((v) => text(v, "id") === id) ?? group.latest;
  const previous = group.versions[group.versions.indexOf(selected) + 1];
  const [against, setAgainst] = useState<string>(text(previous, "id"));
  const other = group.versions.find((v) => text(v, "id") === against && v !== selected);
  const figures = measuredFigures(group);
  const [figure, setFigure] = useState(figures[0]?.key ?? "");
  const series = useMemo(() => scoreSeries(group, figure), [group, figure]);
  const chartData = useMemo(() => [series.map((p) => p.at / 1000), series.map((p) => p.value)], [series]);
  const chartLabels = useMemo(() => [`Held-out ${figures.find((f) => f.key === figure)?.label ?? ""}`], [figures, figure]);
  const runId = text(selected, "run_id") || text(selected, "run");
  const report = useWq<Row>(runId ? "get_run_report" : null, { run_id: runId }, { retry: false });
  const n = (model: Row) => versionNumber(group, text(model, "id"));

  return (
    <Split
      side={
        <>
          <Card label="Facts">
            <KeyValue
              rows={[
                ["Kind", group.kind ? <Chip key="k">{group.kind.replace(/[_-]+/g, " ")}</Chip> : null],
                ["Source", <SourceText key="s" group={group} experiment={experiment} />],
                [
                  "Project",
                  group.projectName && !group.standalone ? (
                    <Link key="p" to={path(`/design/project?${qs({ customer_id: scope.customerId, name: group.projectName })}`)}>
                      {group.projectName}
                    </Link>
                  ) : null,
                ],
                ["Version", <span key="v">v{n(selected)} of {group.versions.length}</span>],
                ["Trained", <span key="t" title={when(text(selected, "created"))}>{relative(text(selected, "created"), Date.now(), "—")}</span>],
                ["Run", runId ? <Link key="r" to={path(`/train/run?${qs({ customer_id: scope.customerId, id: runId })}`)}><Mono short={8}>{runId}</Mono></Link> : null],
                ["Model id", <Mono key="id" short={12}>{text(selected, "id")}</Mono>],
              ]}
            />
          </Card>
          <Weights selected={selected} n={n(selected)} name={group.name} />
          {group.standalone ? <Standalone group={group} /> : <PipelineSteps group={group} experiment={experiment} />}
          <Train group={group} groups={groups} selected={selected} n={n(selected)} />
        </>
      }
    >
      <StatRow cols={3}>
        <Stat label="Held-out score" value={figures[0] ? <ScoreText score={group.score} note={false} /> : "—"} sub={group.score ? "the latest version that measured one" : "no version measured one"} />
        <Stat label="Versions" value={group.versions.length} sub={`${group.runs.length} ${group.runs.length === 1 ? "run" : "runs"}`} />
        <Stat label="Serving" value={group.published.length ? `v${n(group.published[0])}` : "none"} tone={group.published.length ? "ok" : undefined} sub={group.published.length ? "published in Ship" : "no version is published"} to={links.ship(text(group.published[0], "id") || id)} />
      </StatRow>

      <Card
        label="Held-out score over versions"
        actions={
          figures.length > 1 ? (
            <div className="flex flex-wrap gap-1">
              {figures.map((f) => (
                <Chip key={f.key} active={f.key === figure} onClick={() => setFigure(f.key)}>
                  {f.label}
                </Chip>
              ))}
            </div>
          ) : null
        }
      >
        {series.length >= 2 ? (
          <Chart chartKey={`model-${group.key}-${figure}`} data={chartData} labels={chartLabels} settings={{ title: chartLabels[0], xLabel: "Trained", time: true }} />
        ) : (
          <p className="muted m-0 text-[13px]">{series.length === 1 ? "One version measured a held-out score; the chart starts with the second." : "No version measured a held-out score. Retrain with rows held out to measure one."}</p>
        )}
      </Card>

      <Card label="Versions" padded={false}>
        <DataTable<Row>
          rows={group.versions}
          what="versions"
          rowKey={(v) => text(v, "id")}
          columns={[
            {
              key: "version",
              label: "Version",
              render: (v) => (
                <div className="flex flex-wrap items-center gap-1.5">
                  {v === selected ? <strong>v{n(v)}</strong> : <Link to={links.train(text(v, "id"))}>v{n(v)}</Link>}
                  {v === group.latest && <Chip>latest</Chip>}
                  {promoted.has(text(v, "id")) && <Chip tone="ok">promoted</Chip>}
                </div>
              ),
            },
            { key: "trained", label: "Trained", render: (v) => <span title={when(text(v, "created"))}>{relative(text(v, "created"), Date.now(), "—")}</span> },
            { key: "score", label: "Held-out score", render: (v) => <VersionScore model={v} /> },
            {
              key: "run",
              label: "Run",
              render: (v) => {
                const run = text(v, "run_id") || text(v, "run");
                return run ? (
                  <Link to={path(`/train/run?${qs({ customer_id: scope.customerId, id: run })}`)}>
                    <Mono short={8}>{run}</Mono>
                  </Link>
                ) : (
                  <span className="muted">—</span>
                );
              },
            },
            { key: "ship", label: "Serving", render: (v) => (v.exposed === true ? <Status value="published" tone="success" /> : <span className="muted">—</span>) },
            {
              key: "compare",
              label: <span className="sr-only">Compare</span>,
              className: "text-right",
              render: (v) =>
                v === selected ? null : (
                  <Button variant="ghost" onClick={() => setAgainst(text(v, "id"))}>
                    <GitCompare size={14} aria-hidden /> Compare
                  </Button>
                ),
            },
          ]}
        />
      </Card>

      {other && <Comparison group={group} from={other} to={selected} />}
      <Learned selected={selected} report={report} />
    </Split>
  );
}

/** Download the selected version's weights: as Lake keeps them, or a tree model as ONNX
 * (`export_model`, lake#681). */
function Weights({ selected, n, name }: { selected: Row; n: number; name: string }) {
  const scope = useWorkspace();
  const toast = useToast();
  const [busy, setBusy] = useState<"zetta" | "onnx" | null>(null);
  const [error, setError] = useState<unknown>(null);
  const download = async (format: "zetta" | "onnx") => {
    setBusy(format);
    setError(null);
    try {
      const answer = await read<Row>("export_model", { customer_id: scope.customerId, workspace_id: scope.workspaceId, model_id: text(selected, "id"), format });
      const file = weightsFile(answer);
      const url = URL.createObjectURL(new Blob([file.bytes as BlobPart], { type: file.type }));
      const link = document.createElement("a");
      link.href = url;
      link.download = file.name;
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      toast(`Downloaded ${file.name}`);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(null);
    }
  };
  return (
    <Card label="Weights">
      <p className="muted mt-0 mb-2 text-[12.5px]">
        v{n} of {name} as a file, to keep or to score elsewhere.
      </p>
      <div className="flex flex-wrap gap-2">
        <Button loading={busy === "zetta"} disabled={!!busy} onClick={() => void download("zetta")}>
          <Download size={14} aria-hidden /> Download
        </Button>
        {exportsAsOnnx(selected) && (
          <Button loading={busy === "onnx"} disabled={!!busy} onClick={() => void download("onnx")}>
            <Download size={14} aria-hidden /> As ONNX
          </Button>
        )}
      </div>
      <FormError error={error} />
    </Card>
  );
}

/** Two versions side by side: each number either measured, and whether a held-out figure got
 * better. Their training curves open in Compare Runs. */
function Comparison({ group, from, to }: { group: ModelGroup; from: Row; to: Row }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const changes = compareVersions(from, to);
  const a = versionNumber(group, text(from, "id"));
  const b = versionNumber(group, text(to, "id"));
  const runs = [text(from, "run_id") || text(from, "run"), text(to, "run_id") || text(to, "run")].filter(Boolean);
  return (
    <Card
      label="Compare versions"
      title={`v${a} and v${b}`}
      actions={
        runs.length === 2 && runs[0] !== runs[1] ? (
          <Button variant="ghost" to={path(`/train/experiments/compare?${qs({ customer_id: customerId, runs: runs.join(",") })}`)}>
            Training curves
          </Button>
        ) : null
      }
    >
      {changes.length === 0 ? (
        <p className="muted m-0">Neither version recorded a figure.</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th scope="col">Figure</th>
                <th scope="col" className="text-right">
                  v{a}
                </th>
                <th scope="col" className="text-right">
                  v{b}
                </th>
                <th scope="col">Change</th>
              </tr>
            </thead>
            <tbody>
              {changes.map((change) => (
                <tr key={change.key}>
                  <td>{change.label}</td>
                  <td className="text-right font-mono tabular-nums">{change.from === null ? "—" : cellText(change.from)}</td>
                  <td className="text-right font-mono tabular-nums">{change.to === null ? "—" : cellText(change.to)}</td>
                  <td>{change.better === null ? <span className="muted">—</span> : <Chip tone={change.better ? "ok" : "warn"}>{change.better ? "better" : "worse"}</Chip>}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </Card>
  );
}

/** What the version learned from: the dataset its run report names and the features it reads. */
function Learned({ selected, report }: { selected: Row; report: ReturnType<typeof useWq<Row>> }) {
  const features = featuresOf(selected);
  const dataset = report.data?.dataset && typeof report.data.dataset === "object" && !Array.isArray(report.data.dataset) ? (report.data.dataset as Row) : null;
  return (
    <Card label="What it learned from">
      {dataset ? (
        <KeyValue
          rows={Object.entries(dataset).map(([key, value]) => [
            key === "source" ? "Trained on" : key === "not_used" ? "Columns not used" : key.replace(/_/g, " "),
            Array.isArray(value) ? value.map(cellText).join(", ") : cellText(value),
          ])}
        />
      ) : report.isLoading ? (
        <Skeleton lines={2} />
      ) : (
        <p className="muted m-0 text-[13px]">{report.isError ? "The run report could not be read, so its dataset is not shown." : "The run report names no dataset."}</p>
      )}
      {features.length > 0 && (
        <div className="mt-3">
          <div className="label">Features</div>
          <div className="mt-1 flex flex-wrap gap-1">
            {features.map((feature) => (
              <Chip key={feature}>{feature}</Chip>
            ))}
          </div>
        </div>
      )}
    </Card>
  );
}

/** The pipeline step or steps the model belongs to, from the pipeline's graph; or that it has no
 * pipeline. */
function PipelineSteps({ group, experiment }: { group: ModelGroup; experiment?: { id: string; name: string } }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const project = group.projectName || group.project;
  const graph = useWq<Row>(group.pipeline && project ? "get_graph" : null, { project, graph: group.pipeline }, { retry: false });
  const pipelineLink = (step?: string) => path(`/design/pipeline?${qs({ customer_id: customerId, project, graph: group.pipeline, step })}`);
  if (!group.pipeline) {
    return (
      <Card label="Pipeline">
        {experiment ? (
          <p className="m-0 text-[13px]">
            Trained by experiment{" "}
            <Link to={path(`/train/experiment?${qs({ customer_id: customerId, experiment: experiment.id })}`)}>{experiment.name || experiment.id}</Link>, with no pipeline named.
          </p>
        ) : (
          <p className="m-0 text-[13px]">Uploaded, no pipeline.</p>
        )}
      </Card>
    );
  }
  const { steps, exact } = modelSteps(graph.data, group.name);
  return (
    <Card label="Pipeline" title={<Link to={pipelineLink()}>{group.pipeline}</Link>}>
      {graph.isLoading ? (
        <Skeleton lines={2} />
      ) : graph.isError ? (
        <p className="muted m-0 text-[13px]">The pipeline could not be read; it may have been renamed or deleted.</p>
      ) : steps.length === 0 ? (
        <p className="muted m-0 text-[13px]">The pipeline has no model step now.</p>
      ) : (
        <>
          {!exact && <p className="muted mt-0 mb-2 text-[12.5px]">The pipeline does not say which of its model steps trained this model:</p>}
          <ul className="m-0 list-none space-y-1 p-0">
            {steps.map((step) => (
              <li key={step.id} className="flex flex-wrap items-center gap-1.5">
                <Chip>model step</Chip>
                <Link to={pipelineLink(step.id)}>{step.name || step.id}</Link>
              </li>
            ))}
          </ul>
        </>
      )}
    </Card>
  );
}

/** Retrain (run the pipeline again under the model's name) and promote the chosen version to Ship
 * (publish it, and unpublish the other versions so predict answers with this one). */
function Train({ group, groups, selected, n }: { group: ModelGroup; groups: ModelGroup[]; selected: Row; n: number }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const links = useModelLinks();
  const navigate = useNavigate();
  const toast = useToast();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const [versioning, setVersioning] = useState(false);
  const project = group.projectName || group.project;
  const id = text(selected, "id");
  const others = group.published.filter((v) => v !== selected);
  const graph = useWq<Row>(group.standalone && !group.brought && group.pipeline ? "get_graph" : null, { project: group.project, graph: group.pipeline }, { retry: false });
  const retrain = async () => {
    setBusy(true);
    setError(null);
    try {
      if (group.standalone) {
        // A standalone model's pipeline is the engine's: retrain_model runs it on what it trained on.
        const answer = await act<Row>("retrain_model", { customer_id: scope.customerId, workspace_id: scope.workspaceId, model_id: text(group.latest, "id") });
        toast("Retraining started");
        navigate(path(`/train/run?${qs({ customer_id: scope.customerId, id: text(answer, "run_id") })}`));
        return;
      }
      const answer = await act<Row>("run_pipeline", { customer_id: scope.customerId, workspace_id: scope.workspaceId, project, graph: group.pipeline, model_name: group.name });
      const run = answer.run && typeof answer.run === "object" ? text(answer.run as Row, "run_id") : "";
      if (!answer.run) throw new Error(`The pipeline did not pass its checks: ${text((answer.gate ?? {}) as Row, "verdict") || "see the pipeline"}`);
      toast("Retraining started");
      navigate(path(`/train/run?${qs({ customer_id: scope.customerId, id: run })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const promote = async () => {
    await wa(scope, "expose_model", { model_id: id });
    for (const version of others) await wa(scope, "unexpose_model", { model_id: text(version, "id") });
    toast(`v${n} is serving`);
    navigate(links.ship(id));
  };
  return (
    <Card label="Train and promote">
      <div className="space-y-3">
        <div>
          {group.brought ? (
            <Button onClick={() => setVersioning(true)}>
              <Plus size={14} aria-hidden /> New version
            </Button>
          ) : group.pipeline ? (
            <div className="flex flex-wrap gap-2">
              <Button loading={busy} onClick={() => void retrain()}>
                <RefreshCw size={14} aria-hidden /> Retrain
              </Button>
              {group.standalone && (
                <Button onClick={() => setVersioning(true)}>
                  <Plus size={14} aria-hidden /> New version
                </Button>
              )}
            </div>
          ) : (
            <p className="muted m-0 text-[12.5px]">Retraining runs a pipeline again; this model has none.</p>
          )}
          {group.brought ? (
            <p className="muted mt-1 mb-0 text-[12.5px]">Brought from a file: a new file is its next version.</p>
          ) : group.standalone ? (
            <p className="muted mt-1 mb-0 text-[12.5px]">Retrain trains it again on what it learned from; New version on new data, another column or another held-out share.</p>
          ) : (
            group.pipeline && <p className="muted mt-1 mb-0 text-[12.5px]">Runs {group.pipeline} again after its checks, as a new version of this model.</p>
          )}
          <FormError error={error} />
        </div>
        <div>
          {selected.exposed === true && others.length === 0 ? (
            <Banner tone="ok">v{n} is the version serving.</Banner>
          ) : (
            <ConfirmButton
              variant="primary"
              title={`Promote v${n} to Ship?`}
              confirmLabel="Promote"
              confirm={
                <>
                  Publishes v{n}, so <code>predict</code> answers with it.
                  {others.length > 0 && <> It unpublishes {others.map((v) => `v${versionNumber(group, text(v, "id"))}`).join(", ")}: a call naming that id is refused from then on.</>}
                </>
              }
              onConfirm={promote}
            >
              <Rocket size={14} aria-hidden /> Promote v{n} to Ship
            </ConfirmButton>
          )}
        </div>
        {group.standalone && <StandaloneActions group={group} />}
      </div>
      {versioning && group.brought && <BringDialog groups={groups} versionOf={group} onClose={() => setVersioning(false)} />}
      {versioning && !group.brought && <RetrainDialog group={group} training={trainingOf(graph.data)} onClose={() => setVersioning(false)} />}
    </Card>
  );
}

/** What a standalone model trains on (its pipeline's model step keeps it), or where a brought one
 * came from. Its project is the engine's and is never shown. */
function Standalone({ group }: { group: ModelGroup }) {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const graph = useWq<Row>(!group.brought && group.pipeline ? "get_graph" : null, { project: group.project, graph: group.pipeline }, { retry: false });
  const training = trainingOf(graph.data);
  if (group.brought) {
    const source = broughtFrom(group.latest);
    return (
      <Card label="Standalone model">
        <p className="m-0 text-[13px]">Brought from a file{source ? <>: {source}</> : null}. It has no pipeline; each file brought is a version.</p>
      </Card>
    );
  }
  return (
    <Card label="Standalone model">
      {graph.isLoading ? (
        <Skeleton lines={2} />
      ) : training ? (
        <KeyValue
          rows={[
            [
              "Dataset",
              training.dataset ? (
                <Link key="d" to={path(`/train/dataset?${qs({ customer_id: customerId, database: training.dataset.split(".")[0], name: training.dataset.split(".").slice(1).join(".") })}`)}>
                  {training.dataset}
                </Link>
              ) : null,
            ],
            ["Predicts", training.target || <span key="t" className="muted">no target: it learns without one</span>],
            ["Methodology", training.methodology],
            ["Held out", training.holdout !== null ? `${Math.round(training.holdout * 100)}% of rows` : <span key="h" className="muted">none: in-sample figures only</span>],
          ]}
        />
      ) : (
        <p className="muted m-0 text-[13px]">Trained on its own. {graph.isError ? "What it trains on could not be read." : "Its training settings are not recorded."}</p>
      )}
      <p className="muted mb-0 mt-2 text-[12.5px]">No project or pipeline to look after. Move it into a project to build a pipeline around it.</p>
    </Card>
  );
}

/** Move into a project, and Delete: every version, and with the last the model itself. */
function StandaloneActions({ group }: { group: ModelGroup }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const links = useModelLinks();
  const navigate = useNavigate();
  const toast = useToast();
  const [projectName, setProjectName] = useState(group.name);
  const input = { customer_id: scope.customerId, workspace_id: scope.workspaceId };
  const move = async () => {
    const answer = await act<Row>("move_model_to_project", { ...input, model_id: text(group.latest, "id"), project_name: projectName.trim() || group.name });
    toast(`${group.name} is in the project ${text(answer, "project_name")}`);
    navigate(path(`/design/project?${qs({ customer_id: scope.customerId, name: text(answer, "project_name") })}`));
  };
  const remove = async () => {
    // Oldest last: the engine removes the model's own project with its last version.
    for (const version of group.versions) {
      try {
        await act<Row>("delete_model", { ...input, id: text(version, "id") });
      } catch (e) {
        if (!(e instanceof ApiError && e.code === "not_found")) throw e;
      }
    }
    toast(`Deleted ${group.name}`);
    navigate(links.trainList);
  };
  const live = group.published.length > 0;
  return (
    <div className="flex flex-wrap gap-2 border-t border-hairline pt-3">
      <ConfirmButton
        title={`Move ${group.name} into a project?`}
        confirmLabel="Move into project"
        confirm={
          <div className="grid gap-2">
            <p className="m-0">It becomes a project in Design → Projects, with its pipeline and all {group.versions.length} {group.versions.length === 1 ? "version" : "versions"}, so you can build a pipeline around it. It is no longer standalone.</p>
            <div className="field m-0">
              <label htmlFor="move-project-name">Project name</label>
              <input id="move-project-name" value={projectName} onChange={(e) => setProjectName(e.target.value)} />
            </div>
          </div>
        }
        onConfirm={move}
      >
        <FolderInput size={14} aria-hidden /> Move into a project
      </ConfirmButton>
      <ConfirmButton
        variant="ghost"
        danger
        title={`Delete ${group.name}?`}
        confirmLabel="Delete model"
        typed={group.name}
        confirm={
          <>
            Deletes all {group.versions.length} {group.versions.length === 1 ? "version" : "versions"} of {group.name}. It cannot be undone.
            {live && <> A published version stops answering <code>predict</code>.</>} The runs that trained it are kept.
          </>
        }
        onConfirm={remove}
      >
        <Trash2 size={14} aria-hidden /> Delete
      </ConfirmButton>
    </div>
  );
}
