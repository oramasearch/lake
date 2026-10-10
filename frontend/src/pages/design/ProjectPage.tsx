/** Design → Project (lake's workspace_content, project branch): one project's state, its solution
 * when it is one, its five latest runs, the Project Agent's background build or foundation
 * proposal, its pipelines, the ways to start one, and its deletion. While a build, a proposal or a
 * run is active every read is asked again every 3 seconds, as the server page's refresh did. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { act } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { Badge, Button, ErrorBlock, Field, Form, Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath, wa, wq } from "./api";
import { datasetChoices, runIsActive, runsOf, solutionProjectOf, solutionState, taskIsActive, text, withoutProjectAgentChecks, type J } from "./logic";
import { FormError, Panel, Refusal, RunsPanel, entityCrumb, useDesignTrail, useSubmit } from "./parts";
import { ActiveSolution, ProposalContent } from "./SolutionPanels";

export default function ProjectPage() {
  const [params] = useSearchParams();
  const name = params.get("name") ?? "";
  useDesignTrail({ label: "Projects", path: "/design/projects" }, [name ? entityCrumb(name) : { label: "Project" }], name ? `Project: ${name}` : "Project");
  return (
    <Page eyebrow="Design" title={name ? `Project: ${name}` : "Project"}>
      <WorkspaceGate>
        <ProjectBody key={name} name={name} />
      </WorkspaceGate>
    </Page>
  );
}

type Task = J & { status?: string; message?: string; build_error?: string };

function ProjectBody({ name }: { name: string }) {
  const scope = useScope();
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

  if (!name) return <div className="panel empty">No project named.</div>;
  if (project.isLoading) return <Skeleton />;
  if (project.isError) return <ErrorBlock error={project.error} retry={() => void project.refetch()} what="this project" />;

  const solution = solutionProjectOf(graphs.map((summary, index) => ({ summary, detail: details[index]?.data as J | undefined })));
  const solutionRuns = solution ? runsOf(allRuns, solution.graph, solution.graphId) : [];
  const state = solutionState(solutionRuns);
  const buildStatus = (buildTask?.status ?? "").toLowerCase();

  return (
    <>
      {build.isError && (
        <p className="error" role="alert">
          The Project Agent's design could not be read: {(build.error as Error).message}
        </p>
      )}
      {buildTask?.build_error && (
        <p className="error" role="alert">
          The Project Agent designed a pipeline, but it could not be applied: {buildTask.build_error}
        </p>
      )}
      {proposal.isError && (
        <p className="error" role="alert">
          The Project Agent's foundation proposal could not be read: {(proposal.error as Error).message}
        </p>
      )}
      {solution ? (
        <p className={state.failed ? "sub error" : "sub"}>
          <strong>{state.lead}</strong>
          {state.rest ? ` ${state.rest}` : ""}
        </p>
      ) : (
        <p className="sub">{text(project.data ?? {}, "description") || "No description."}</p>
      )}
      {solution ? (
        <ActiveSolution project={name} solution={solution} task={solutionTask} runs={solutionRuns} live={live} />
      ) : solutionTask ? (
        <ProposalContent project={name} task={solutionTask} />
      ) : null}
      {runs.isError ? <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" /> : runs.isLoading ? <Skeleton /> : <RunsPanel runs={allRuns} />}
      {designing && (
        <section className="panel notice" role="status">
          <p>
            <strong>The Project Agent is designing this project's pipeline…</strong> It reads your data and the Opportunity this project came from; the pipeline appears here
            when it is done. You can leave this page. This page checks again every 3 seconds.
          </p>
          <p>{buildTask?.message ?? ""}</p>
        </section>
      )}
      {!designing && graphs.length === 0 && buildStatus === "failed" && (
        <p className="error" role="alert">
          The Project Agent could not design this project's pipeline: {buildTask?.message ?? ""} Describe it below instead.
        </p>
      )}
      <Pipelines project={name} graphs={graphs} designing={designing} />
      <BuildPipeline project={name} datasets={datasets} methodologies={methodologies.data} />
      <OtherWays project={name} />
      <DeleteProject project={name} />
    </>
  );
}

function Pipelines({ project, graphs, designing }: { project: string; graphs: J[]; designing: boolean }) {
  const ws = useWsPath();
  return (
    <Panel title="Pipelines">
      {graphs.length === 0 && designing ? (
        <p>Designing…</p>
      ) : graphs.length === 0 ? (
        <p>No pipelines yet. Describe what the project should predict or answer, and Zetta builds one.</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Pipeline</th>
                <th>Methodology</th>
                <th>Approval</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {graphs.map((graph) => {
                const id = text(graph, "id");
                const graphName = text(graph, "name") || id;
                const methodology = text(graph, "methodology");
                return (
                  <tr key={id || graphName}>
                    <td>
                      <Link to={ws(`/design/pipeline?${qs({ project, graph: graphName })}`)}>{graphName}</Link>
                    </td>
                    <td>
                      {"runnable" in graph ? (
                        graph.runnable === true ? (
                          <Badge tone="success">{methodology || "Runs"}</Badge>
                        ) : (
                          <Badge tone="warning">{`Trains nothing yet${methodology ? `: needs ${methodology}` : ""}`}</Badge>
                        )
                      ) : (
                        "—"
                      )}
                    </td>
                    <td>{text(graph, "approval")}</td>
                    <td>
                      <PipelineAction project={project} graphId={id} approved={text(graph, "approval") === "approved"} />
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
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
    <div>
      <Button loading={busy} onClick={() => void submit()}>
        {approved ? "Check and run" : "Approve"}
      </Button>
      <FormError error={error} />
    </div>
  );
}

function BuildPipeline({ project, datasets, methodologies }: { project: string; datasets: ReturnType<typeof useWq<J>>; methodologies: J | undefined }) {
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
    });
  return (
    <Panel title="Build a Pipeline">
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="What should it predict or answer?" required>
          <textarea name="intent" rows={3} placeholder="Predict which accounts will churn in the next 90 days, with the reasons" value={intent} onChange={(e) => setIntent(e.target.value)} />
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
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Build pipeline
          </Button>
        </div>
      </Form>
    </Panel>
  );
}

function OtherWays({ project }: { project: string }) {
  return (
    <Panel title="Other Ways to Start a Pipeline">
      <div className="pipeline-edit">
        <CreatePipeline project={project} from="empty" />
        <CreatePipeline project={project} from="n8n" />
        <CreatePipeline project={project} from="python" />
      </div>
    </Panel>
  );
}

/** An empty pipeline, one imported from n8n, or one lowered from Python (create_pipeline); a new
 * pipeline opens on its own page. */
function CreatePipeline({ project, from }: { project: string; from: "empty" | "n8n" | "python" }) {
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
          <Field label="Name" required>
            <input name="name" placeholder="weekly-retention" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <p className="muted">An empty pipeline you build step by step.</p>
          <div>
            <Button type="submit" loading={busy}>
              Create
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
            <textarea name="content" rows={5} value={content} onChange={(e) => setContent(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={busy}>
              Import from n8n
            </Button>
          </div>
        </>
      )}
      {from === "python" && (
        <>
          <Field label="Name">
            <input name="name" placeholder="python program" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <Field label="Python pipeline" required>
            <textarea name="source" rows={5} value={content} onChange={(e) => setContent(e.target.value)} />
          </Field>
          <p className="muted">Zetta reads it and lowers it to a pipeline; the code never runs.</p>
          <div>
            <Button type="submit" loading={busy}>
              Build from Python
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
  const [confirm, setConfirm] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      if (confirm !== project) throw new Refusal("Type the project's name to delete it.");
      await wa(scope, "delete_project", { name: project });
      toast(`Project ${project} deleted`);
      navigate(ws("/design/projects"));
    });
  return (
    <section className="panel">
      <details>
        <summary>Delete project</summary>
        <Form onSubmit={submit}>
          <FormError error={error} />
          <p>Removes the project, its intent and its pipelines. Its runs and models stay listed.</p>
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
          <div>
            <Button type="submit" variant="danger" loading={busy}>
              Delete project
            </Button>
          </div>
        </Form>
      </details>
    </section>
  );
}
