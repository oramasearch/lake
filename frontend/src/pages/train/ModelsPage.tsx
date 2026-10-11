/** Train → Models (lake#599): every model the workspace trained, as a model with its versions: the
 * latest held-out score, the runs and experiments that trained it, the features it learned from,
 * and whether a version is serving. A row opens the model's training page; serving is Ship →
 * Models. A model uploaded on its own is a model repository (Data → Repositories). */
import { useMemo, useState } from "react";
import { Link } from "react-router-dom";
import { Brain, FlaskConical, Globe, Layers, Sparkles } from "lucide-react";
import { relative } from "@/lib/format";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Chip, DataTable, Empty, FilterChips, Mono, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { WorkspaceFrame, qs, text, useWorkspace, useWsPath } from "./workspace";
import { when } from "./RunsPage";
import { datasetOf, featuresOf, type ModelGroup } from "./models";
import { ScoreText, SourceText, VersionChip, useModelGroups, useModelLinks } from "./modelParts";

export default function TrainModelsPage() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const space = qs({ customer_id: customerId });
  useTrail([
    { label: "Train", to: path(`/train/agents?${space}`) },
    { label: "Models", to: path(`/train/models?${space}`) },
  ]);
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title="Models"
      sub="Every model this workspace trained: its versions, held-out scores and what it learned from."
      actions={
        <Button variant="primary" to={path(`/design/projects?${space}`)}>
          <Sparkles size={14} aria-hidden /> Train a model
        </Button>
      }
    >
      {() => <Models />}
    </WorkspaceFrame>
  );
}

type Source = "pipeline" | "experiment" | "none";

function Models() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  const links = useModelLinks();
  const { q, groups, experiments } = useModelGroups();
  const [needle, setNeedle] = useState("");
  const [source, setSource] = useState<Source | null>(null);
  const experimentOf = (group: ModelGroup) => {
    for (const version of group.versions) {
      const ref = experiments.byModel.get(text(version, "id")) ?? experiments.byRun.get(text(version, "run"));
      if (ref) return ref;
    }
    return undefined;
  };
  const sourceOf = (group: ModelGroup): Source => (group.pipeline ? "pipeline" : experimentOf(group) ? "experiment" : "none");
  const counts = useMemo(() => {
    const out = { pipeline: 0, experiment: 0, none: 0 };
    for (const group of groups) out[sourceOf(group)] += 1;
    return out;
    // sourceOf reads the experiments the groups were made from.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [groups]);
  const lower = needle.trim().toLowerCase();
  const rows = groups.filter((group) => {
    if (source && sourceOf(group) !== source) return false;
    if (!lower) return true;
    const words = [group.name, group.kind, group.pipeline, group.projectName, experimentOf(group)?.name ?? "", ...featuresOf(group.latest), ...group.versions.map((v) => text(v, "id"))];
    return words.join(" ").toLowerCase().includes(lower);
  });
  const versions = groups.reduce((sum, group) => sum + group.versions.length, 0);
  const scored = groups.filter((group) => group.score).length;
  const serving = groups.filter((group) => group.published.length).length;
  const projects = path(`/design/projects?${qs({ customer_id: customerId })}`);

  return (
    <>
      {q.data && groups.length > 0 && (
        <>
          <StatRow>
            <Stat label="Models" value={groups.length} icon={Brain} sub={`${versions} trained ${versions === 1 ? "version" : "versions"}`} />
            <Stat label="Held-out score" value={scored} icon={Layers} sub="measured on rows the model did not see" />
            <Stat label="From experiments" value={counts.experiment} icon={FlaskConical} sub={`${counts.pipeline} from pipelines`} />
            <Stat label="Serving" value={serving} icon={Globe} tone={serving ? "ok" : undefined} sub="a version published in Ship" to={links.shipList} />
          </StatRow>
          <Toolbar>
            <SearchInput value={needle} onChange={setNeedle} placeholder="Search models, features or ids…" label="Search models" />
            <FilterChips<Source>
              value={source}
              onChange={setSource}
              allLabel="Every source"
              options={[
                { id: "pipeline", label: "Pipeline", count: counts.pipeline },
                { id: "experiment", label: "Experiment", count: counts.experiment },
                { id: "none", label: "No pipeline", count: counts.none },
              ]}
            />
          </Toolbar>
        </>
      )}
      <DataTable<ModelGroup>
        q={q}
        what="models"
        caption="Models"
        rows={rows}
        rowKey={(group) => group.key}
        initialSort={{ key: "trained", dir: "descending" }}
        empty={
          groups.length === 0 ? (
            <Empty icon={Brain} title="No models yet" action={<Button to={projects}>Open projects</Button>}>
              Run a pipeline with a model step, or start an experiment, and its model appears here.
            </Empty>
          ) : (
            <Empty compact title="No model matches">
              Clear the search or the filter to see more.
            </Empty>
          )
        }
        columns={[
          {
            key: "model",
            label: "Model",
            sort: (group) => group.name.toLowerCase(),
            render: (group) => (
              <div className="min-w-0">
                <div className="flex flex-wrap items-center gap-1.5">
                  <Link className="font-semibold" to={links.train(text(group.latest, "id"))}>
                    {group.name}
                  </Link>
                  {group.kind && <Chip>{group.kind.replace(/[_-]+/g, " ")}</Chip>}
                  {group.versions.some((v) => experiments.promoted.has(text(v, "id"))) && <Chip tone="ok">promoted</Chip>}
                </div>
                <small className="muted">
                  <SourceText group={group} experiment={experimentOf(group)} />
                </small>
              </div>
            ),
          },
          {
            key: "score",
            label: "Latest held-out score",
            sort: (group) => group.score?.value ?? -1,
            render: (group) => <ScoreText score={group.score} />,
          },
          {
            key: "versions",
            label: "Trained by",
            sort: (group) => group.versions.length,
            render: (group) => {
              const experiment = experimentOf(group);
              return (
                <div className="text-[12.5px]">
                  <span className="font-mono tabular-nums">{group.versions.length}</span> {group.versions.length === 1 ? "version" : "versions"},{" "}
                  <span className="font-mono tabular-nums">{group.runs.length}</span> {group.runs.length === 1 ? "run" : "runs"}
                  {group.runs[0] && (
                    <>
                      <br />
                      <Link to={path(`/train/run?${qs({ customer_id: customerId, id: group.runs[0] })}`)}>
                        latest run <Mono short={8}>{group.runs[0]}</Mono>
                      </Link>
                    </>
                  )}
                  {experiment && (
                    <>
                      <br />
                      <Link to={path(`/train/experiment?${qs({ customer_id: customerId, experiment: experiment.id })}`)}>experiment {experiment.name || experiment.id}</Link>
                    </>
                  )}
                </div>
              );
            },
          },
          {
            key: "features",
            label: "Learned from",
            sort: (group) => featuresOf(group.latest).length,
            render: (group) => {
              const features = featuresOf(group.latest);
              const dataset = datasetOf(group.latest);
              if (!features.length && !dataset) return <span className="muted">in the run report</span>;
              return (
                <div className="min-w-0">
                  {dataset && <div className="text-[12.5px]">{dataset}</div>}
                  <div className="flex flex-wrap gap-1">
                    {features.slice(0, 3).map((feature) => (
                      <Chip key={feature}>{feature}</Chip>
                    ))}
                    {features.length > 3 && <Chip>+{features.length - 3}</Chip>}
                  </div>
                </div>
              );
            },
          },
          {
            key: "trained",
            label: "Last trained",
            sort: (group) => text(group.latest, "created"),
            render: (group) => <span title={when(text(group.latest, "created"))}>{relative(text(group.latest, "created"), Date.now(), "—")}</span>,
          },
          {
            key: "ship",
            label: "Serving",
            sort: (group) => group.published.length,
            render: (group) => {
              const live = group.published[0];
              if (!live) return <Status value="not published" tone="neutral" />;
              return (
                <Link to={links.ship(text(live, "id"))} className="inline-flex items-center gap-1.5 no-underline">
                  <Status value="published" tone="success" />
                  <VersionChip n={group.versions.length - group.versions.indexOf(live)} />
                </Link>
              );
            },
          },
        ]}
      />
      {q.data && (
        <div className="mt-4">
          <Banner tone="quiet" title="Uploaded Models">
            A model uploaded on its own, with no pipeline, is a model repository: open it from <Link to="/repositories">Repositories</Link>. Lake does not list model repositories on this page yet.
          </Banner>
        </div>
      )}
    </>
  );
}
