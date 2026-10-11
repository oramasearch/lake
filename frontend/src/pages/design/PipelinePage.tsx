/** Design → Pipeline (lake's pipeline_content): see, inspect, change, govern and follow one Zetta
 * execution graph. The graph is the server's own drawing (`render_pipeline`, inserted as the SVG
 * markup it answers); selecting a step keeps it in the URL (`step=`). A Project Agent turn is
 * followed by its task (`task=`), polled every 3 seconds while it works. Gate and deploy answers
 * are shown once, never put in the URL. */
import { type MouseEvent, type ReactNode, useEffect, useRef, useState } from "react";
import { Link, useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { act, type Row } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { Check, MousePointerClick } from "lucide-react";
import { Banner, Button, Chip, DataTable, Empty, ErrorBlock, Field, Form, KeyValue, Page, Skeleton, Split, Status } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath, wa } from "./api";
import {
  EDGE_KINDS,
  STEP_KINDS,
  cellText,
  datasetChoices,
  defaultModelName,
  graphOf,
  parseNumber,
  plainAgentFailure,
  projectAgentNeedsReply,
  runsOf,
  taskIsActive,
  text,
  whenUtc,
  type Graph,
  type J,
} from "./logic";
import { FormError, GatePanel, Identifier, Markdown, Panel, PipelineSvg, Refusal, RunBadge, When, entityCrumb, useDesignTrail, useSubmit } from "./parts";

const SUB = "The steps Zetta runs and how data flows between them. A changed pipeline is approved again before it runs.";

export default function PipelinePage() {
  const [params] = useSearchParams();
  const project = params.get("project") ?? "";
  const graph = params.get("graph") ?? "";
  const ws = useWsPath();
  useDesignTrail(
    { label: "Projects", path: "/design/projects" },
    [project ? entityCrumb(project, ws(`/design/project?${qs({ name: project })}`)) : null, entityCrumb(graph || "Pipeline")],
    graph ? `Pipeline: ${graph}` : "Pipeline",
  );
  // The same read the body makes: the header carries the approval.
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const read = useWq<J>("get_graph", { project, graph }, { enabled: ready });
  const approval = graphOf(read.data)?.approval;
  return (
    <Page
      eyebrow={project ? `Design · Pipeline of ${project}` : "Design · Pipeline"}
      title={
        <span className="inline-flex flex-wrap items-center gap-2">
          {graph || "Pipeline"} {approval && <Status value={approval === "approved" ? "approved" : "proposed"} tone={approval === "approved" ? "success" : "warning"} />}
        </span>
      }
      sub={SUB}
    >
      <WorkspaceGate>
        <PipelineBody key={`${project}\u0000${graph}`} project={project} graphName={graph} />
      </WorkspaceGate>
    </Page>
  );
}

/** What a Check, Run or Deploy answered, shown until the next one. */
type Shown = { gate?: J; deploy?: J };

function PipelineBody({ project, graphName }: { project: string; graphName: string }) {
  const ws = useWsPath();
  const [params] = useSearchParams();
  const location = useLocation();
  const navigate = useNavigate();
  const step = params.get("step") ?? "";
  const taskId = params.get("task") ?? "";
  const [shown, setShown] = useState<Shown>({});

  const read = useWq<J>("get_graph", { project, graph: graphName });
  const runs = useWq<J>("list_runs");
  const datasets = useWq<J>("list_datasets");
  const svg = useScopedRead<{ svg?: string }>("render_pipeline", { project, graph: graphName, step: step || undefined }, { placeholderData: (previous) => previous });
  const task = useScopedRead<J>(taskId ? "get_agent_task" : null, { task_id: taskId }, {
    refetchInterval: (query) => (taskIsActive(text((query.state.data as J | undefined) ?? {}, "status")) ? 3000 : false),
  });

  // Land on #proposal or #run once the content it names is on the page.
  const scrolled = useRef("");
  useEffect(() => {
    const id = location.hash.slice(1);
    if (!id || scrolled.current === location.key || read.isLoading) return;
    const target = document.getElementById(id);
    if (target) {
      target.scrollIntoView({ block: "start" });
      scrolled.current = location.key;
    }
  });

  if (read.isLoading) return <Skeleton />;
  if (read.isError) return <ErrorBlock error={read.error} retry={() => void read.refetch()} what="this pipeline" />;
  const graph = graphOf(read.data);
  if (!graph) {
    return (
      <Empty title="Pipeline Not Readable" action={<Button to={ws(`/design/project?${qs({ name: project })}`)}>Back to the project</Button>}>
        This pipeline could not be read from the workspace.
      </Empty>
    );
  }

  const pipelineRuns = runsOf(runs.data?.runs, graph.name, graph.graph_id);
  const latest = pipelineRuns[0];
  const lastRunOf = new Map<string, string>();
  for (const id of Array.isArray(latest?.lineage?.steps) ? latest!.lineage.steps : []) if (typeof id === "string") lastRunOf.set(id, text(latest!, "status"));

  /** Change the URL in place: the graph's name after a rename, the step selected, the task followed. */
  const go = (changes: Record<string, string | null>, hash = "") => {
    const next = new URLSearchParams(params);
    for (const [key, value] of Object.entries(changes)) {
      if (value) next.set(key, value);
      else next.delete(key);
    }
    navigate({ search: `?${next.toString()}`, hash: hash ? `#${hash}` : "" });
  };
  const approved = graph.approval === "approved";
  const runnable = read.data && "runnable" in read.data;
  const methodology = text(read.data ?? {}, "methodology");

  const selected = step && graph.steps.some((x) => x.id === step) ? step : "";
  return (
    <div className="space-y-4">
      {runnable && read.data!.runnable !== true && (
        <Banner tone="quiet" title="This Pipeline Trains Nothing Yet">
          {methodology ? `It needs ${methodology}, which this workspace's engine does not run yet.` : "It has no model step the engine can run, so a run only passes through its steps."}{" "}
          Build a pipeline with a methodology that runs, or ask the Project Agent to change it.
        </Banner>
      )}
      {shown.gate && <GatePanel gate={shown.gate} heading="Checks" />}
      {shown.deploy && (
        <>
          <Banner tone="ok" title="Deployed">
            Deployment <code>{text(shown.deploy, "deployment_id")}</code> to <code>{text(shown.deploy, "target")}</code>.
          </Banner>
          <GatePanel gate={shown.deploy.gate ?? {}} heading="Its Checks" />
        </>
      )}
      {taskId && <ProjectAgentTurn project={project} graph={graph} task={task.data} taskError={task.error} taskLoading={task.isLoading} onDiscard={() => go({ task: null })} />}
      <Split
        side={
          <>
            {selected ? (
              <SelectedStep key={selected} project={project} graph={graph} stepId={selected} lastRun={lastRunOf.get(selected)} go={go} />
            ) : (
              <Panel label="Step">
                <Empty compact icon={MousePointerClick} title="Select a Step">
                  Select a step in the graph to inspect or change it.
                </Empty>
              </Panel>
            )}
            <Panel label="Pipeline" title={graph.name}>
              <KeyValue
                rows={[
                  ["Project", <ProjectLink key="p" project={project} />],
                  [
                    "Approval",
                    <span key="a" className="inline-flex flex-wrap items-center gap-2">
                      <Status value={approved ? "approved" : "proposed"} tone={approved ? "success" : "warning"} />
                      {approved ? (graph.approved_at ? <span className="text-[12px] text-ink-3">{whenUtc(graph.approved_at)}</span> : null) : <span className="text-[12px] text-ink-3">approve before it runs</span>}
                    </span>,
                  ],
                  ["Steps", <span key="s" className="font-mono">{graph.steps.length}</span>],
                  ["Edges", <span key="e" className="font-mono">{graph.edges.length}</span>],
                  ...(runnable && read.data!.runnable === true ? ([["Trains with", <Chip key="t" tone="ok">{methodology || "its model step"}</Chip>]] as [string, ReactNode][]) : []),
                  ["Latest run", latest ? <RunBadge key="l" status={text(latest, "status")} /> : "none yet"],
                ]}
              />
            </Panel>
            <Govern
              project={project}
              graph={graph}
              approved={approved}
              datasets={datasetChoices(datasets.data?.datasets).map((d) => d.path)}
              dataset={params.get("dataset") ?? ""}
              holdout={params.get("holdout") ?? ""}
              onShown={setShown}
            />
          </>
        }
      >
        <Panel title="Graph" label={`${graph.steps.length} steps · ${graph.edges.length} edges`} footer="Solid arrows carry data; dashed arrows only order steps. Select a step to inspect or change it.">
          {graph.steps.length === 0 ? (
            <Empty compact title="No Steps Yet">
              Add the first one under Change the Pipeline.
            </Empty>
          ) : svg.isError ? (
            <ErrorBlock error={svg.error} retry={() => void svg.refetch()} what="the graph" />
          ) : !svg.data?.svg ? (
            <Skeleton />
          ) : (
            <ServerSvg markup={svg.data.svg} onStep={(id) => go({ step: id })} />
          )}
        </Panel>
        <AskProjectAgent project={project} graph={graph.name} onAsked={(id) => go({ task: id }, "proposal")} />
        <ChangePipeline project={project} graph={graph} go={go} />
        <Panel title="Runs">
          {runs.isError ? (
            <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" />
          ) : runs.isLoading ? (
            <Skeleton />
          ) : (
            <RunsTable runs={pipelineRuns} />
          )}
        </Panel>
      </Split>
    </div>
  );
}

function ProjectLink({ project }: { project: string }) {
  const ws = useWsPath();
  return <Link to={ws(`/design/project?${qs({ name: project })}`)}>{project}</Link>;
}

function RunsTable({ runs }: { runs: J[] }) {
  const ws = useWsPath();
  return (
    <DataTable<J>
      rows={runs}
      caption="Runs of this pipeline"
      rowKey={(run, index) => text(run, "id") || String(index)}
      empty={<p className="m-0 text-[13px] text-ink-2">No runs of this pipeline yet.</p>}
      columns={[
        {
          key: "id",
          label: "Run",
          render: (run) => (
            <Link to={ws(`/train/run?${qs({ id: text(run, "id") })}`)}>
              <Identifier value={text(run, "id")} />
            </Link>
          ),
        },
        { key: "status", label: "Status", sort: (run) => text(run, "status"), render: (run) => <RunBadge status={text(run, "status")} /> },
        { key: "created", label: "Started", sort: (run) => text(run, "created"), render: (run) => <When value={text(run, "created")} /> },
        { key: "outputs", label: "Outputs", numeric: true, render: (run) => <span className="font-mono">{text(run, "output_count") || "—"}</span> },
      ]}
    />
  );
}

/** The server's SVG, inserted as it is. Its steps are links ending `step=<id>`; a click selects
 * the step in the page instead of loading the server's page. */
function ServerSvg({ markup, onStep }: { markup: string; onStep: (id: string) => void }) {
  const onClick = (e: MouseEvent<HTMLDivElement>) => {
    const anchor = (e.target as Element).closest("a");
    if (!anchor || e.metaKey || e.ctrlKey || e.shiftKey || e.button !== 0) return;
    const href = anchor.getAttribute("href") ?? anchor.getAttribute("xlink:href") ?? "";
    const match = /[?&]step=([^&#]*)/.exec(href);
    if (!match) return;
    e.preventDefault();
    onStep(decodeURIComponent(match[1].replace(/\+/g, " ")));
  };
  return <div className="pipeline-canvas" onClick={onClick} dangerouslySetInnerHTML={{ __html: markup }} />;
}

/* ------------------------------------------------------------------ the Project Agent */

function ProjectAgentTurn({ project, graph, task, taskError, taskLoading, onDiscard }: { project: string; graph: Graph; task: J | undefined; taskError: unknown; taskLoading: boolean; onDiscard: () => void }) {
  if (taskLoading) return <Skeleton />;
  if (taskError) return <ErrorBlock error={taskError} what="the Project Agent's turn" />;
  if (!task) return null;
  const status = text(task, "status");
  if (taskIsActive(status)) {
    return (
      <div id="proposal" className="scroll-mt-4">
        <Banner title="The Project Agent is working on it…">
          It reads your data and prepares the edits; nothing changes until you apply them. This page checks again every 3 seconds.
          {text(task, "message") && <div className="mt-1">{text(task, "message")}</div>}
        </Banner>
      </div>
    );
  }
  const answer = task.answer && typeof task.answer === "object" ? task.answer : null;
  const proposal = answer?.result?.proposal;
  if (proposal && typeof proposal === "object" && !Array.isArray(proposal)) {
    return <Proposal project={project} graph={graph} proposal={proposal} onDiscard={onDiscard} />;
  }
  // No proposal: a question to answer, or why it proposed nothing.
  const said = answer && "result" in answer ? (typeof answer.result?.reply === "string" ? answer.result.reply : "no proposal") : text(task, "message") || "no proposal";
  if (projectAgentNeedsReply(task)) return <AgentQuestion project={project} graph={graph.name} task={task} question={said} />;
  return (
    <div id="proposal" className="scroll-mt-4">
      <Banner tone="warn" title="The Project Agent did not propose a change">
        {plainAgentFailure(said)} Ask it again with more detail, or change the steps by hand.
      </Banner>
    </div>
  );
}

function Proposal({ project, graph, proposal, onDiscard }: { project: string; graph: Graph; proposal: J; onDiscard: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const { busy, error, run } = useSubmit();
  const steps: unknown[] = Array.isArray(proposal.steps) ? proposal.steps : [];
  const problems = [...new Set((Array.isArray(proposal.problems) ? proposal.problems : []).map((p: unknown) => plainAgentFailure(cellText(p))))];
  const settings: J = proposal.run && typeof proposal.run === "object" ? proposal.run : {};
  const holdout = typeof settings.holdout === "number" ? settings.holdout : null;
  const after = graphOf(proposal.after && typeof proposal.after === "object" ? { ...proposal.after, name: graph.name } : null);
  const applicable = problems.length === 0 && steps.length > 0;
  const apply = () =>
    run(async () => {
      const edits = Array.isArray(proposal.edits) ? proposal.edits : [];
      const answer = await act<J>("edit_pipeline", { customer_id: scope.customerId, workspace_id: scope.workspaceId, project, graph: graph.name, edits });
      // The Run form is prefilled with the proposal's settings.
      navigate(ws(`/design/pipeline?${qs({ project, graph: text(answer, "name") || graph.name, dataset: text(settings, "dataset"), holdout: holdout === null ? "" : String(holdout) })}#run`));
    });
  return (
    <Panel id="proposal" tone="active" label="Proposed by the Project Agent" title={text(proposal, "summary")}>
      <p className="mt-0 text-[12.5px] text-ink-2">Nothing has changed yet. Applying saves these edits; the pipeline then needs approval again before it runs.</p>
      {steps.length === 0 ? (
        <p className="text-[13px] text-ink-2">No edits to the graph.</p>
      ) : (
        <>
          <h3>Edits</h3>
          <ol>
            {steps.map((s, index) => (
              <li key={index}>{cellText(s)}</li>
            ))}
          </ol>
        </>
      )}
      {(text(settings, "dataset") || holdout !== null) && (
        <>
          <h3>Run settings</h3>
          <ul>
            {text(settings, "dataset") && (
              <li>
                Train on <code>{text(settings, "dataset")}</code>
              </li>
            )}
            {holdout !== null && <li>Hold out {(holdout * 100).toFixed(0)}% of rows for measuring</li>}
          </ul>
        </>
      )}
      {problems.length > 0 && (
        <>
          <h3>Why it cannot be applied</h3>
          <ul>
            {problems.map((p) => (
              <li key={p}>{p}</li>
            ))}
          </ul>
          <p className="text-[13px] text-ink-2">Ask the Project Agent again, or change the steps by hand.</p>
        </>
      )}
      {after && after.steps.length > 0 && (
        <>
          <h3>After</h3>
          <div className="pipeline-canvas">
            <PipelineSvg graph={after} />
          </div>
        </>
      )}
      <FormError error={error} />
      <div className="actions">
        {applicable && (
          <Button variant="primary" loading={busy} onClick={() => void apply()}>
            Apply
          </Button>
        )}
        <Button onClick={onDiscard}>Discard</Button>
      </div>
    </Panel>
  );
}

function AgentQuestion({ project, graph, task, question }: { project: string; graph: string; task: J; question: string }) {
  const scope = useScope();
  const [params, setParams] = useSearchParams();
  const [reply, setReply] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      // The reply continues the same conversation: only an agent-project pipeline task that asks
      // something, and carries its conversation, can be answered.
      const conversation = text(task, "conversation_id");
      if (text(task, "agent") !== "agent-project" || text(task, "kind") !== "pipeline" || !projectAgentNeedsReply(task) || !conversation) {
        throw new Refusal("That task has no Project Agent question to answer.");
      }
      const answer = await act<J>("agent_turn", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        agent: "agent-project",
        message: reply,
        kind: "pipeline",
        context: { project, graph },
        conversation_id: conversation,
      });
      setReply("");
      const next = new URLSearchParams(params);
      next.set("task", text(answer, "task_id"));
      setParams(next);
    });
  return (
    <Panel id="proposal" tone="active" title="Project Agent Question">
      <div className="ask-reply text-[13.5px]">
        <Markdown>{question}</Markdown>
      </div>
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="Reply" required>
          <textarea name="reply" rows={3} value={reply} onChange={(e) => setReply(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Reply to the Project Agent
          </Button>
        </div>
      </Form>
    </Panel>
  );
}

function AskProjectAgent({ project, graph, onAsked }: { project: string; graph: string; onAsked: (taskId: string) => void }) {
  const scope = useScope();
  const [request, setRequest] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const answer = await act<J>("agent_turn", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        agent: "agent-project",
        message: request,
        kind: "pipeline",
        context: { project, graph },
      });
      setRequest("");
      onAsked(text(answer, "task_id"));
    });
  return (
    <Panel title="Ask the Project Agent" label="It proposes edits for you to review">
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="What should change?" required>
          <textarea name="request" rows={3} placeholder="Train on ux_review_churn only and hold out 20% of rows" value={request} onChange={(e) => setRequest(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Propose a change
          </Button>
        </div>
      </Form>
      <p className="mb-0 mt-2 text-[12.5px] text-ink-3">It reads your data and proposes edits for you to review; nothing changes until you apply them.</p>
    </Panel>
  );
}

/* ------------------------------------------------------------------ editing */

type Go = (changes: Record<string, string | null>, hash?: string) => void;

/** One edit_pipeline call; afterwards the URL names the pipeline by its (possibly new) name and
 * selects the step the server's handler selected. */
function useEdit(project: string, graph: Graph, go: Go) {
  const scope = useScope();
  const toast = useToast();
  const submit = useSubmit();
  const edit = (input: Row, select: (answer: J) => string | null) =>
    submit.run(async () => {
      const answer = await act<J>("edit_pipeline", { customer_id: scope.customerId, workspace_id: scope.workspaceId, project, graph: graph.name, ...input });
      const name = text(answer, "name") || graph.name;
      go({ graph: name, step: select(answer) });
      toast("Pipeline saved");
    });
  return { ...submit, edit };
}

function KindOptions() {
  return (
    <>
      {STEP_KINDS.map((k) => (
        <option key={k} value={k}>
          {k}
        </option>
      ))}
    </>
  );
}

function EdgeKindOptions() {
  return (
    <>
      {EDGE_KINDS.map((k) => (
        <option key={k} value={k}>
          {k}
        </option>
      ))}
    </>
  );
}

function SelectedStep({ project, graph, stepId, lastRun, go }: { project: string; graph: Graph; stepId: string; lastRun: string | undefined; go: Go }) {
  const s = graph.steps.find((x) => x.id === stepId)!;
  const incoming = graph.edges.filter((e) => e.to === s.id);
  const outgoing = graph.edges.filter((e) => e.from === s.id);
  const others = graph.steps.filter((x) => x.id !== s.id);
  const rename = useEdit(project, graph, go);
  const kind = useEdit(project, graph, go);
  const connect = useEdit(project, graph, go);
  const remove = useEdit(project, graph, go);
  const [name, setName] = useState(s.name);
  const [stepKind, setStepKind] = useState(STEP_KINDS.includes(s.kind as (typeof STEP_KINDS)[number]) ? s.kind : "connect");
  const [to, setTo] = useState(others[0]?.id ?? "");
  const [edgeKind, setEdgeKind] = useState("data_flow");
  const select = (id: string) => (
    <a
      href={`?${qs({ project, graph: graph.name, step: id })}`}
      onClick={(e) => {
        e.preventDefault();
        go({ step: id });
      }}
    >
      {id}
    </a>
  );
  return (
    <Panel label={`Step · ${s.kind}`} title={s.name} actions={<code className="identifier">{s.id}</code>}>
      <KeyValue
        className="mb-3"
        rows={[
          ["Kind", s.kind],
          ...Object.entries(s.attributes).map(([key, value]) => [key, <code key={key}>{typeof value === "string" ? value : JSON.stringify(value)}</code>] as [string, ReactNode]),
          ["Last run", lastRun ? <RunBadge key="lr" status={lastRun} /> : "not in the last run"],
          [
            "From",
            incoming.length === 0 ? (
              <span key="f" className="text-ink-3">nothing</span>
            ) : (
              <ul key="f" className="m-0 list-none space-y-0.5 pl-0">
                {incoming.map((e) => (
                  <li key={`${e.from}>${e.to}`}>
                    {select(e.from)} <span className="text-[12px] text-ink-3">{e.kind}</span>
                  </li>
                ))}
              </ul>
            ),
          ],
          [
            "To",
            outgoing.length === 0 ? (
              <span key="t" className="text-ink-3">nothing</span>
            ) : (
              <ul key="t" className="m-0 list-none space-y-0.5 pl-0">
                {outgoing.map((e) => (
                  <li key={`${e.from}>${e.to}`}>
                    {select(e.to)} <span className="text-[12px] text-ink-3">{e.kind}</span>
                  </li>
                ))}
              </ul>
            ),
          ],
        ]}
      />
      <div className="pipeline-edit">
        <Form onSubmit={() => rename.edit({ op: "rename_step", step: s.id, name }, () => s.id)}>
          <FormError error={rename.error} />
          <Field label="Name" required>
            <input name="name" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={rename.busy}>
              Rename step
            </Button>
          </div>
        </Form>
        <Form onSubmit={() => kind.edit({ op: "kind_step", step: s.id, kind: stepKind }, () => s.id)}>
          <FormError error={kind.error} />
          <Field label="Kind">
            <select name="kind" value={stepKind} onChange={(e) => setStepKind(e.target.value)}>
              <KindOptions />
            </select>
          </Field>
          <div>
            <Button type="submit" loading={kind.busy}>
              Change kind
            </Button>
          </div>
        </Form>
        <Form onSubmit={() => connect.edit({ op: "add_edge", from: s.id, to, kind: edgeKind }, () => s.id)}>
          <FormError error={connect.error} />
          <Field label="Then">
            <select name="to" value={to} onChange={(e) => setTo(e.target.value)}>
              {others.map((o) => (
                <option key={o.id} value={o.id}>
                  {o.name} ({o.id})
                </option>
              ))}
            </select>
          </Field>
          <Field label="Carrying">
            <select name="kind" value={edgeKind} onChange={(e) => setEdgeKind(e.target.value)}>
              <EdgeKindOptions />
            </select>
          </Field>
          <div>
            <Button type="submit" loading={connect.busy}>
              Connect
            </Button>
          </div>
        </Form>
        <div className="stack">
          <FormError error={remove.error} />
          <p className="m-0 text-[12.5px] text-ink-2">Removing a step removes its edges too.</p>
          <div>
            <Button variant="danger" loading={remove.busy} onClick={() => void remove.edit({ op: "remove_step", step: s.id }, () => null)}>
              Remove step
            </Button>
          </div>
        </div>
      </div>
    </Panel>
  );
}

function ChangePipeline({ project, graph, go }: { project: string; graph: Graph; go: Go }) {
  const add = useEdit(project, graph, go);
  const edge = useEdit(project, graph, go);
  const rename = useEdit(project, graph, go);
  const removeEdge = useEdit(project, graph, go);
  const [kind, setKind] = useState("model");
  const [name, setName] = useState("");
  const [from, setFrom] = useState(graph.steps[0]?.id ?? "");
  const [to, setTo] = useState(graph.steps[0]?.id ?? "");
  const [edgeKind, setEdgeKind] = useState("data_flow");
  const [newName, setNewName] = useState(graph.name);
  const stepOptions = graph.steps.map((s) => (
    <option key={s.id} value={s.id}>
      {s.name} ({s.id})
    </option>
  ));
  const select = (id: string) => (
    <a
      href={`?${qs({ project, graph: graph.name, step: id })}`}
      onClick={(e) => {
        e.preventDefault();
        go({ step: id });
      }}
    >
      {id}
    </a>
  );
  return (
    <Panel title="Change the Pipeline">
      <div className="pipeline-edit">
        <Form onSubmit={() => add.edit({ op: "add_step", kind, ...(name ? { name } : {}) }, (answer) => text(answer, "step") || null)}>
          <FormError error={add.error} />
          <Field label="Kind">
            <select name="kind" value={kind} onChange={(e) => setKind(e.target.value)}>
              <KindOptions />
            </select>
          </Field>
          <Field label="Name">
            <input name="name" placeholder="Score accounts" value={name} onChange={(e) => setName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" variant="primary" loading={add.busy}>
              Add step
            </Button>
          </div>
        </Form>
        <Form onSubmit={() => edge.edit({ op: "add_edge", from, to, kind: edgeKind }, () => from || null)}>
          <FormError error={edge.error} />
          <Field label="From">
            <select name="from" value={from} onChange={(e) => setFrom(e.target.value)}>
              {stepOptions}
            </select>
          </Field>
          <Field label="To">
            <select name="to" value={to} onChange={(e) => setTo(e.target.value)}>
              {stepOptions}
            </select>
          </Field>
          <Field label="Carrying">
            <select name="kind" value={edgeKind} onChange={(e) => setEdgeKind(e.target.value)}>
              <EdgeKindOptions />
            </select>
          </Field>
          <div>
            <Button type="submit" loading={edge.busy}>
              Add edge
            </Button>
          </div>
        </Form>
        <Form onSubmit={() => rename.edit({ op: "rename", name: newName }, () => null)}>
          <FormError error={rename.error} />
          <Field label="Name" required>
            <input name="name" value={newName} onChange={(e) => setNewName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" loading={rename.busy}>
              Rename pipeline
            </Button>
          </div>
        </Form>
      </div>
      {graph.edges.length > 0 && (
        <>
          <h3>Edges</h3>
          <FormError error={removeEdge.error} />
          <DataTable
            rows={graph.edges}
            caption="Edges"
            rowKey={(e) => `${e.from}>${e.to}`}
            columns={[
              { key: "from", label: "From", render: (e) => select(e.from) },
              { key: "to", label: "To", render: (e) => select(e.to) },
              { key: "kind", label: "Carrying", render: (e) => <span className="text-ink-2">{e.kind}</span> },
              {
                key: "remove",
                label: "",
                className: "text-right",
                render: (e) => (
                  <Button variant="ghost" loading={removeEdge.busy} onClick={() => void removeEdge.edit({ op: "remove_edge", from: e.from, to: e.to }, () => null)}>
                    Remove
                  </Button>
                ),
              },
            ]}
          />
        </>
      )}
    </Panel>
  );
}

/* ------------------------------------------------------------------ governing */

/** Approve, Check, Run and Deploy. Run and Deploy wait for approval. A run that starts opens its
 * page; a run the checks blocked, a check and a deploy show their answer here once. */
function Govern({
  project,
  graph,
  approved,
  datasets,
  dataset,
  holdout,
  onShown,
}: {
  project: string;
  graph: Graph;
  approved: boolean;
  datasets: string[];
  dataset: string;
  holdout: string;
  onShown: (shown: Shown) => void;
}) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const approve = useSubmit();
  const check = useSubmit();
  const runIt = useSubmit();
  const deploy = useSubmit();
  const [trainOn, setTrainOn] = useState(dataset);
  const [holdoutFraction, setHoldoutFraction] = useState(holdout || "0.2");
  const [modelName, setModelName] = useState(() => defaultModelName(project, graph.name));
  const target = { project, graph: graph.name };
  const top = () => window.scrollTo({ top: 0, behavior: "smooth" });
  return (
    <Panel id="run" title="Approve, Check, Run and Deploy" tone={approved ? "default" : "active"}>
      <div className="pipeline-edit">
        {/* Approved, the cell stays where it was with the same footprint: removing it reflowed the
            grid and shortened the page under a reader at its foot, which moved the whole layout
            (lake#578). */}
        <Form
          onSubmit={() =>
            approved
              ? undefined
              : approve.run(async () => {
                  await wa(scope, "approve_plan", target);
                  toast("Pipeline approved");
                })
          }
        >
          <FormError error={approve.error} />
          <p className="m-0 text-[12.5px] text-ink-2">
            {approved
              ? `Approved${graph.approved_at ? ` ${whenUtc(graph.approved_at)}` : ""}. lake verifies and runs only an approved pipeline.`
              : "lake verifies and runs only an approved pipeline."}
          </p>
          <div>
            {approved ? (
              <Button disabled>
                <Check size={14} aria-hidden /> Approved
              </Button>
            ) : (
              <Button type="submit" variant="primary" loading={approve.busy}>
                Approve
              </Button>
            </div>
          </Form>
        )}
        <Form
          onSubmit={() =>
            check.run(async () => {
              const gate = await wa<J>(scope, "gate", target);
              onShown({ gate });
              top();
            })
          }
        >
          <FormError error={check.error} />
          <p className="m-0 text-[12.5px] text-ink-2">Run Zetta's correctness checks without running anything.</p>
          <div>
            <Button type="submit" loading={check.busy}>
              Check
            </Button>
          </div>
        </Form>
        <Form
          onSubmit={() =>
            runIt.run(async () => {
              // An unreadable hold-out is left out, as the form handler did.
              const fraction = parseNumber(holdoutFraction);
              const answer = await act<J>("run_pipeline", {
                customer_id: scope.customerId,
                workspace_id: scope.workspaceId,
                ...target,
                ...(trainOn ? { dataset_path: trainOn } : {}),
                ...(modelName ? { model_name: modelName } : {}),
                ...(fraction !== null ? { holdout_fraction: fraction } : {}),
              });
              const runId = answer.run && typeof answer.run === "object" ? text(answer.run, "run_id") : "";
              if (answer.run) {
                navigate(ws(`/train/run?${qs({ id: runId })}`));
                return;
              }
              onShown({ gate: answer.gate ?? {} });
              top();
            })
          }
        >
          <FormError error={runIt.error} />
          <Field label="Train on">
            <select name="dataset_path" value={trainOn} onChange={(e) => setTrainOn(e.target.value)}>
              <option value="">the pipeline's own source</option>
              {datasets.map((path) => (
                <option key={path} value={path}>
                  {path}
                </option>
              ))}
            </select>
          </Field>
          <Field label="Hold out for measuring" help="The share of rows kept out of training to measure the model; 0 measures nothing.">
            <input name="holdout_fraction" type="number" min={0} max={0.9} step={0.05} value={holdoutFraction} onChange={(e) => setHoldoutFraction(e.target.value)} />
          </Field>
          <Field label="Model name">
            <input name="model_name" value={modelName} onChange={(e) => setModelName(e.target.value)} />
          </Field>
          <div>
            <Button type="submit" variant="primary" loading={runIt.busy} disabled={!approved}>
              Check and run
            </Button>
          </div>
        </Form>
        <Form
          onSubmit={() =>
            deploy.run(async () => {
              const answer = await wa<J>(scope, "deploy", { ...target, target: "local" });
              onShown({ deploy: answer });
              top();
            })
          }
        >
          <FormError error={deploy.error} />
          <p className="m-0 text-[12.5px] text-ink-2">Place it in the workspace behind its checks.</p>
          <div>
            <Button type="submit" loading={deploy.busy} disabled={!approved}>
              Deploy
            </Button>
          </div>
        </Form>
      </div>
    </Panel>
  );
}
