/** Train → Model (`/train/model?id=`, lake#599): training one model. Its versions (one per run or
 * trial) with their held-out scores over time, two versions compared, what the chosen version
 * learned from, the pipeline step or steps it belongs to, retraining (its pipeline run again) and
 * promoting a version to Ship. Serving it is Ship → Model, linked from the header. */
import { useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ArrowUpRight, GitCompare, RefreshCw, Rocket } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, FormError, KeyValue, Mono, Skeleton, Split, Stat, StatRow, Status } from "@/lib/ui";
import { Chart } from "../experiments/Chart";
import { WorkspaceFrame, cellText, qs, text, useWorkspace, useWq, useWsPath, wa } from "./workspace";
import { when } from "./RunsPage";
import { compareVersions, featuresOf, groupOf, measuredFigures, modelSteps, scoreSeries, versionNumber, type ModelGroup } from "./models";
import { ScoreText, SourceText, VersionScore, useModelGroups, useModelLinks } from "./modelParts";

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
        return <ModelBody key={id} group={group} id={id} experiment={experiment} promoted={experiments.promoted} />;
      }}
    </WorkspaceFrame>
  );
}

function ModelBody({ group, id, experiment, promoted }: { group: ModelGroup; id: string; experiment?: { id: string; name: string }; promoted: Set<string> }) {
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
                  group.projectName ? (
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
          <PipelineSteps group={group} experiment={experiment} />
          <Train group={group} selected={selected} n={n(selected)} />
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
function Train({ group, selected, n }: { group: ModelGroup; selected: Row; n: number }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const links = useModelLinks();
  const navigate = useNavigate();
  const toast = useToast();
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const project = group.projectName || group.project;
  const id = text(selected, "id");
  const others = group.published.filter((v) => v !== selected);
  const retrain = async () => {
    setBusy(true);
    setError(null);
    try {
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
          {group.pipeline ? (
            <Button loading={busy} onClick={() => void retrain()}>
              <RefreshCw size={14} aria-hidden /> Retrain
            </Button>
          ) : (
            <p className="muted m-0 text-[12.5px]">Retraining runs a pipeline again; this model has none.</p>
          )}
          {group.pipeline && <p className="muted mt-1 mb-0 text-[12.5px]">Runs {group.pipeline} again after its checks, as a new version of this model.</p>}
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
      </div>
    </Card>
  );
}
