/** Design → Project (lake's workspace_content, project branch): one project's state, its solution
 * when it is one, its five latest runs, the Project Agent's background build or foundation
 * proposal, its pipelines, the ways to start one, and its deletion. Main column: tabs (Overview,
 * Pipelines, Runs, Solution) kept in the address as `tab=`; side: status, decision, dataset and the
 * actions. Build a pipeline opens a drawer; deletion asks for the typed name in a dialog. While a
 * build, a proposal or a run is active every read is asked again every 3 seconds, as the server
 * page's refresh did. */
import { type ReactNode, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { FileCode2, FolderKanban, Plus, Sparkles, Trash2, Workflow } from "lucide-react";
import { act } from "@/lib/api";
import { Drawer, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Chip, DataTable, Empty, ErrorBlock, Field, Form, KeyValue, Page, Skeleton, Split, StateTabs, Status } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath, wa, wq } from "./api";
import {
  PROJECT_STATE_TONE,
  datasetChoices,
  projectState,
  runIsActive,
  runsOf,
  solutionProjectOf,
  solutionState,
  taskIsActive,
  text,
  withoutProjectAgentChecks,
  type J,
} from "./logic";
import { FormError, Panel, QueryTabs, Refusal, RunBadge, RunsPanel, When, entityCrumb, useDesignTrail, useQueryTab, useSubmit } from "./parts";
import { ActiveSolution, ProposalContent } from "./SolutionPanels";
import "@/styles/design.css";

export default function ProjectPage() {
  const [params] = useSearchParams();
  const name = params.get("name") ?? "";
  useDesignTrail({ label: "Projects", path: "/design/projects" }, [name ? entityCrumb(name) : { label: "Project" }], name ? `Project: ${name}` : "Project");
  // The same read the body makes: the header carries the project's status.
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const project = useWq<J>("get_project", { name }, { enabled: ready && !!name });
  const status = project.data ? projectState(withoutProjectAgentChecks<J>(project.data.graphs)).state : null;
  const [building, setBuilding] = useState(false);
  return (
    <Page
      eyebrow="Design · Project"
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          {name || "Project"} {status && <Status value={status} tone={PROJECT_STATE_TONE[status]} />}
        </span>
      }
      actions={
        ready && name && project.data ? (
          <Button variant="primary" onClick={() => setBuilding(true)}>
            <Plus size={14} aria-hidden /> Build pipeline
          </Button>
        ) : undefined
      }
    >
      <WorkspaceGate>
        <ProjectBody key={name} name={name} building={building} onBuilding={setBuilding} />
      </WorkspaceGate>
    </Page>
  );
}

type Task = J & { status?: string; message?: string; build_error?: string };
const TABS = ["overview", "pipelines", "runs", "solution"] as const;
type Tab = (typeof TABS)[number];

function ProjectBody({ name, building, onBuilding }: { name: string; building: boolean; onBuilding: (open: boolean) => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const [live, setLive] = useState(false);
  const interval = live ? 3000 : false;

  // The Project Agent's background tasks for this project (lake's latest_agent_task): reading a
  // finished build applies its design to the project's pipeline.
  const build = useScopedRead<{ task: Task | null }>("latest_agent_task", { subject: `build:${name}` }, { refetchInterval: interval, enabled: !!name });
  const proposal = useScopedRead<{ task: Task | null }>("latest_agent_task", { subject: `solution:${name}` }, { refetchInterval: interval, enabled: !!name });
  const project = useWq<J>("get_project", { name }, { refetchInterval: interval, enabled: !!name });
  const datasets = useWq<J>("list_datasets");
  const methodologies = useWq<J>("list_methodologies");
  const runs = useScopedRead<{ runs?: J[] }>("list_runs", { project: name, limit: 5 }, { refetchInterval: interval, enabled: !!name });

  const graphs = withoutProjectAgentChecks<J>(project.data?.graphs);
  const details = useQueries({
    queries: graphs.map((graph) => ({
      queryKey: ["workspace_query", scope.customerId, scope.workspaceId, "get_graph", { project: name, graph: text(graph, "name") }],
      queryFn: ({ signal }: { signal: AbortSignal }) => wq<J>(scope, "get_graph", { project: name, graph: text(graph, "name") }, signal),
      refetchInterval: interval,
    })),
  });

  const buildTask = build.data?.task ?? null;
  const solutionTask = proposal.data?.task ?? null;
  const allRuns: J[] = Array.isArray(runs.data?.runs) ? runs.data!.runs! : [];
  const designing = graphs.length === 0 && !!buildTask && taskIsActive(buildTask.status);
  const nowLive = designing || (!!solutionTask && taskIsActive(solutionTask.status)) || allRuns.some(runIsActive);
  if (nowLive !== live) setLive(nowLive);

  const solution = solutionProjectOf(graphs.map((summary, index) => ({ summary, detail: details[index]?.data as J | undefined })));
  const hasSolution = !!solution || !!solutionTask;
  const fallback: Tab = hasSolution ? "solution" : "overview";
  const tab = useQueryTab<Tab>(hasSolution ? TABS : TABS.filter((t) => t !== "solution"), fallback);

  if (!name) {
    return (
      <Empty icon={FolderKanban} title="No Project Named" action={<Button to={ws("/design/projects")}>All projects</Button>}>
        Open a project from the Projects list.
      </Empty>
    );
  }
  if (project.isLoading) return <Skeleton />;
  if (project.isError) return <ErrorBlock error={project.error} retry={() => void project.refetch()} what="this project" />;

  const solutionRuns = solution ? runsOf(allRuns, solution.graph, solution.graphId) : [];
  const state = solutionState(solutionRuns);
  const buildStatus = (buildTask?.status ?? "").toLowerCase();
  const p = project.data ?? {};
  const summary = projectState(graphs);
  const latest = allRuns[0];
  const dataset = (solution ? text(solution.foundation, "dataset") : "") || text(latest ?? {}, "dataset");

  const side = (
    <>
      <Panel label="Status" title={<Status value={summary.state} tone={PROJECT_STATE_TONE[summary.state]} />} footer={live ? "This page checks again every 3 seconds while work is under way." : undefined}>
        <KeyValue
          rows={[
            ["Pipelines", <span key="p" className="font-mono">{summary.pipelines}</span>],
            ["Approved", <span key="a" className="font-mono">{summary.approved}</span>],
            [
              "Latest run",
              latest ? (
                <span key="r" className="inline-flex flex-wrap items-center gap-2">
                  <RunBadge status={text(latest, "status")} /> <When value={text(latest, "created")} />
                </span>
              ) : (
                "none yet"
              ),
            ],
            ...(buildTask ? ([["Project Agent", <Status key="b" value={text(buildTask, "status")} />]] as [string, ReactNode][]) : []),
            ...(solution ? ([["Solution", <span key="s">{state.lead}</span>]] as [string, ReactNode][]) : []),
          ]}
        />
      </Panel>
      <Panel label="Decision" title="What It Should Improve">
        <p className="m-0 text-[13px]">{text(p, "intent") || text(p, "description") || <span className="text-ink-3">No description.</span>}</p>
        {text(p, "intent") && text(p, "description") && <p className="mb-0 mt-2 text-[12.5px] text-ink-2">{text(p, "description")}</p>}
      </Panel>
      <Panel label="Dataset">
        <p className="m-0 font-mono text-[12.5px] break-all">{dataset || <span className="font-sans text-ink-3">Chosen when you build or run a pipeline.</span>}</p>
      </Panel>
      <Panel label="Actions">
        <div className="flex flex-wrap gap-2">
          <Button to={ws("/design/opportunities")}>
            <Sparkles size={14} aria-hidden /> Opportunities
          </Button>
          <DeleteProject project={name} />
        </div>
      </Panel>
    </>
  );

  return (
    <div className="space-y-4">
      {build.isError && <Banner tone="warn">The Project Agent's design could not be read: {(build.error as Error).message}</Banner>}
      {buildTask?.build_error && <Banner tone="warn">The Project Agent designed a pipeline, but it could not be applied: {buildTask.build_error}</Banner>}
      {proposal.isError && <Banner tone="warn">The Project Agent's foundation proposal could not be read: {(proposal.error as Error).message}</Banner>}
      {designing && (
        <Banner title="The Project Agent is designing this project's pipeline…">
          It reads your data and the Opportunity this project came from; the pipeline appears here when it is done. You can leave this page. This page checks again every 3
          seconds.
          {buildTask?.message ? <div className="mt-1">{buildTask.message}</div> : null}
        </Banner>
      )}
      {!designing && graphs.length === 0 && buildStatus === "failed" && (
        <Banner
          tone="warn"
          title="The Project Agent could not design this project's pipeline"
          action={
            <Button onClick={() => onBuilding(true)}>
              <Plus size={14} aria-hidden /> Describe it
            </Button>
          }
        >
          {buildTask?.message ?? ""} Describe it instead.
        </Banner>
      )}
      <Split side={side}>
        <QueryTabs<Tab>
          value={tab}
          fallback={fallback}
          tabs={[
            { id: "overview", label: "Overview" },
            { id: "pipelines", label: "Pipelines", count: graphs.length },
            { id: "runs", label: "Runs", count: runs.data ? allRuns.length : undefined },
            ...(hasSolution ? [{ id: "solution" as Tab, label: "Solution" }] : []),
          ]}
        />
        {tab === "overview" && (
          <>
            <Panel label="Plan" title={solution ? state.lead : "Overview"} tone={solution && state.failed ? "exception" : "default"}>
              <p className={solution && state.failed ? "m-0 text-[13px] text-bad" : "m-0 text-[13px] text-ink-2"}>
                {solution ? state.rest : text(p, "description") || "No description."}
              </p>
              {!solution && (
                <p className="mb-0 mt-2 text-[13px] text-ink-2">
                  {graphs.length === 0
                    ? designing
                      ? "Designing…"
                      : "No pipelines yet. Describe what the project should predict or answer, and Zetta builds one."
                    : summary.approved === 0
                      ? "Zetta verifies and runs only an approved pipeline: approve one on the Pipelines tab."
                      : `${summary.approved} of ${summary.pipelines} pipelines approved.`}
                </p>
              )}
            </Panel>
            {graphs.length > 0 && <Pipelines project={name} graphs={graphs} designing={designing} onBuild={() => onBuilding(true)} />}
            {runs.isError ? <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" /> : runs.isLoading ? <Skeleton /> : <RunsPanel runs={allRuns} title="Latest Runs" />}
          </>
        )}
        {tab === "pipelines" && (
          <>
            <Pipelines project={name} graphs={graphs} designing={designing} onBuild={() => onBuilding(true)} />
            <OtherWays project={name} />
          </>
        )}
        {tab === "runs" &&
          (runs.isError ? <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" /> : runs.isLoading ? <Skeleton /> : <RunsPanel runs={allRuns} />)}
        {tab === "solution" &&
          (solution ? (
            <ActiveSolution project={name} solution={solution} task={solutionTask} runs={solutionRuns} live={live} />
          ) : solutionTask ? (
            <ProposalContent project={name} task={solutionTask} />
          ) : null)}
      </Split>
      {building && <BuildPipeline project={name} datasets={datasets} methodologies={methodologies.data} onClose={() => onBuilding(false)} />}
    </div>
  );
}

function Pipelines({ project, graphs, designing, onBuild }: { project: string; graphs: J[]; designing: boolean; onBuild: () => void }) {
  const ws = useWsPath();
  return (
    <Panel
      title="Pipelines"
      actions={
        <Button onClick={onBuild}>
          <Plus size={14} aria-hidden /> Build pipeline
        </Button>
      }
    >
      <DataTable<J>
        rows={graphs}
        caption="Pipelines"
        rowKey={(graph) => text(graph, "id") || text(graph, "name")}
        empty={
          <Empty compact icon={Workflow} title={designing ? "Designing…" : "No Pipelines Yet"}>
            {designing ? "The Project Agent is designing this project's pipeline." : "Describe what the project should predict or answer, and Zetta builds one."}
          </Empty>
        }
        columns={[
          {
            key: "name",
            label: "Pipeline",
            sort: (graph) => text(graph, "name"),
            render: (graph) => {
              const graphName = text(graph, "name") || text(graph, "id");
              return (
                <div className="min-w-0">
                  <Link to={ws(`/design/pipeline?${qs({ project, graph: graphName })}`)} className="font-semibold">
                    {graphName}
                  </Link>
                  {text(graph, "id") && <div className="font-mono text-[11px] text-ink-3">{text(graph, "id")}</div>}
                </div>
              );
            },
          },
          {
            key: "methodology",
            label: "Methodology",
            render: (graph) => {
              const methodology = text(graph, "methodology");
              if (!("runnable" in graph)) return <span className="text-ink-3">—</span>;
              return graph.runnable === true ? <Chip tone="ok">{methodology || "Runs"}</Chip> : <Chip tone="warn">{`Trains nothing yet${methodology ? `: needs ${methodology}` : ""}`}</Chip>;
            },
          },
          { key: "approval", label: "Approval", sort: (graph) => text(graph, "approval"), render: (graph) => <Status value={text(graph, "approval")} tone={text(graph, "approval") === "approved" ? "success" : "warning"} /> },
          {
            key: "action",
            label: "",
            className: "text-right",
            render: (graph) => <PipelineAction project={project} graphId={text(graph, "id")} approved={text(graph, "approval") === "approved"} />,
          },
        ]}
      />
    </Panel>
  );
}

/** Approve a proposed pipeline, or check and run an approved one (Zetta verifies and runs only an
 * approved plan). A run that starts opens Train → Runs, as the form did. */
function PipelineAction({ project, graphId, approved }: { project: string; graphId: string; approved: boolean }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      if (!approved) {
        await wa(scope, "approve_plan", { project, graph: graphId });
        toast("Pipeline approved");
        return;
      }
      const answer = await act<J>("run_pipeline", { customer_id: scope.customerId, workspace_id: scope.workspaceId, project, graph: graphId });
      if (!answer.run) throw new Refusal(`The pipeline did not pass its checks: ${text(answer.gate ?? {}, "verdict")}`);
      navigate(ws("/train/runs"));
    });
  return (
    <div className="flex flex-col items-end gap-1">
      <Button variant={approved ? "secondary" : "primary"} loading={busy} onClick={() => void submit()}>
        {approved ? "Check and run" : "Approve"}
      </Button>
      <FormError error={error} />
    </div>
  );
}

function BuildPipeline({ project, datasets, methodologies, onClose }: { project: string; datasets: ReturnType<typeof useWq<J>>; methodologies: J | undefined; onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [intent, setIntent] = useState("");
  const [dataset, setDataset] = useState("");
  const [methodology, setMethodology] = useState("");
  const { busy, error, run } = useSubmit();
  const choices = datasetChoices(datasets.data?.datasets);
  // The methodologies the engine runs (lake#289); absent on an older engine.
  const runnable: J[] = (Array.isArray(methodologies?.methodologies) ? methodologies!.methodologies : []).filter((m: J) => m?.runnable === true);
  const submit = () =>
    run(async () => {
      const answer = await act<J>("compile_pipeline", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        project,
        intent,
        ...(dataset ? { dataset } : {}),
        ...(methodology ? { methodology } : {}),
      });
      const graph = text(answer, "graph");
      setIntent("");
      if (dataset && graph) {
        // Pointed at a dataset: open the pipeline at its Run form, the dataset chosen.
        navigate(ws(`/design/pipeline?${qs({ project, graph, dataset })}#run`));
        return;
      }
      toast(graph ? `Pipeline ${graph} built` : "Pipeline built");
      onClose();
    });
  return (
    <Drawer
      title="Build a Pipeline"
      subtitle="Describe what it should predict or answer; Zetta builds the steps."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="build-pipeline" variant="primary" loading={busy}>
            Build pipeline
          </Button>
        </>
      }
    >
      <Form id="build-pipeline" onSubmit={submit}>
        <FormError error={error} />
        <Field label="What should it predict or answer?" required>
          <textarea name="intent" rows={4} placeholder="Predict which accounts will churn in the next 90 days, with the reasons" value={intent} onChange={(e) => setIntent(e.target.value)} />
        </Field>
        <Field label="From data" help="Choose a dataset and the pipeline reads it, and its runs train on it." error={datasets.isError ? `The datasets could not be listed: ${(datasets.error as Error).message}` : undefined}>
          <select name="dataset" value={dataset} onChange={(e) => setDataset(e.target.value)}>
            <option value="">Let the builder describe its sources</option>
            {choices.map((d) => (
              <option key={d.path} value={d.path}>
                {d.name}
              </option>
            ))}
          </select>
        </Field>
        {runnable.length > 0 && (
          <Field label="Methodology" help="Only the methodologies this workspace's engine runs are listed.">
            <select name="methodology" value={methodology} onChange={(e) => setMethodology(e.target.value)}>
              <option value="">Let Zetta choose from the description</option>
              {runnable.map((m) => (
                <option key={text(m, "id")} value={text(m, "id")}>
                  {text(m, "name")} ({text(m, "id")})
                </option>
              ))}
            </select>
          </Field>
        )}
      </Form>
    </Drawer>
  );
}

type From = "empty" | "n8n" | "python";

function OtherWays({ project }: { project: string }) {
  const [from, setFrom] = useState<From>("empty");
  return (
    <Panel title="Other Ways to Start a Pipeline">
      <StateTabs<From>
        value={from}
        onChange={setFrom}
        label="Start from"
        tabs={[
          { value: "empty", label: "Empty" },
          { value: "n8n", label: "From n8n" },
          { value: "python", label: "From Python" },
        ]}
      />
      <CreatePipeline key={from} project={project} from={from} />
    </Panel>
  );
}

/** An empty pipeline, one imported from n8n, or one lowered from Python (create_pipeline); a new
 * pipeline opens on its own page. */
function CreatePipeline({ project, from }: { project: string; from: From }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [name, setName] = useState("");
  const [content, setContent] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const answer = await act<J>("create_pipeline", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        project,
        from,
        ...(name ? { name } : {}),
        ...(from !== "empty" ? { content } : {}),
      });
      navigate(ws(`/design/pipeline?${qs({ project, graph: text(answer, "name") || name })}`));
    });
  return (
    <Form onSubmit={submit}>
      <FormError error={error} />
      {from === "empty" && (
        <>
          <Field label="Name" required help="An empty pipeline you build step by step.">
            <input name="name" placeholder="weekly-retention" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={busy}>
              <Plus size={14} aria-hidden /> Create
            </Button>
          </div>
        </>
      )}
      {from === "n8n" && (
        <>
          <Field label="Name">
            <input name="name" placeholder="n8n workflow" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="n8n workflow export (JSON)" required>
            <textarea name="content" rows={6} className="font-mono" value={content} onChange={(e) => setContent(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={busy}>
              <FileCode2 size={14} aria-hidden /> Import from n8n
            </Button>
          </div>
        </>
      )}
      {from === "python" && (
        <>
          <Field label="Name">
            <input name="name" placeholder="python program" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Python pipeline" required help="Zetta reads it and lowers it to a pipeline; the code never runs.">
            <textarea name="source" rows={6} className="font-mono" value={content} onChange={(e) => setContent(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={busy}>
              <FileCode2 size={14} aria-hidden /> Build from Python
            </Button>
          </div>
        </>
      )}
    </Form>
  );
}

/** Delete the project, once its name is typed: the check the form handler made, which the
 * capability does not. */
function DeleteProject({ project }: { project: string }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [open, setOpen] = useState(false);
  const [confirm, setConfirm] = useState("");
  const { busy, error, run, setError } = useSubmit();
  const close = () => {
    setOpen(false);
    setConfirm("");
    setError(null);
  };
  const submit = () =>
    run(async () => {
      if (confirm !== project) throw new Refusal("Type the project's name to delete it.");
      await wa(scope, "delete_project", { name: project });
      toast(`Project ${project} deleted`);
      navigate(ws("/design/projects"));
    });
  return (
    <>
      <Button variant="danger" onClick={() => setOpen(true)}>
        <Trash2 size={14} aria-hidden /> Delete project
      </Button>
      {open && (
        <Modal
          title={`Delete ${project}?`}
          onClose={close}
          width={480}
          footer={
            <>
              <Button onClick={close}>Cancel</Button>
              <Button type="submit" form="delete-project" variant="danger" loading={busy}>
                Delete project
              </Button>
            </>
          }
        >
          <Form id="delete-project" onSubmit={submit}>
            <FormError error={error} />
            <p className="m-0 text-[13px] text-ink-2">Removes the project, its intent and its pipelines. Its runs and models stay listed.</p>
            <Field
              label={
                <>
                  Type <code>{project}</code> to delete it
                </>
              }
              required
            >
              <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
            </Field>
          </Form>
        </Modal>
      )}
    </>
  );
}
