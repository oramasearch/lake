/** Train → Agent (web_ui.cpp agent_content): one agent, its pipelines as tools, its schedules, its
 * unattended runs, its training and Delete. Each control calls the capability its form handler
 * called (http_server.cpp agent_grant … agent_delete). */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ApiError, act, read, type Row } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Button, DataTable, ErrorBlock, Field, Skeleton } from "@/lib/ui";
import { engagement, modelLabel, skillLabels } from "./agents";
import { WorkspaceFrame, qs, text, useAsync, useSpaceRead, useWorkspace, useWq, useWsPath, wa, wq } from "./workspace";

export default function AgentPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const query = qs({ customer_id: customerId });
  const agent = useWq<Row>(id ? "get_agent" : null, { id });
  const name = agent.data ? text(agent.data, "name") : "";
  useTrail(
    [{ label: "Train", to: ws(`/train/agents?${query}`) }, { label: "Agents", to: ws(`/train/agents?${query}`) }, entityCrumb(id || "Agent")],
    `Agent: ${name || id}`,
  );
  return (
    <WorkspaceFrame eyebrow="Train" title={`Agent: ${name || id}`}>
      {() => {
        if (!id) return <div className="panel empty">No such agent in this workspace.</div>;
        if (agent.isLoading) return <Skeleton />;
        if (agent.isError) {
          if (agent.error instanceof ApiError && agent.error.status === 404) return <div className="panel empty">No such agent in this workspace.</div>;
          return <ErrorBlock error={agent.error} retry={() => void agent.refetch()} what="the agent" />;
        }
        if (!agent.data || !Object.keys(agent.data).length) return <div className="panel empty">No such agent in this workspace.</div>;
        return <AgentContent agent={agent.data} />;
      }}
    </WorkspaceFrame>
  );
}

function AgentContent({ agent }: { agent: Row }) {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const id = text(agent, "id");
  const runtime = text(agent, "runtime");
  const area = (agent.work_area && typeof agent.work_area === "object" ? agent.work_area : {}) as Row;
  let areaText = text(area, "kind") === "project" ? "A single project" : text(area, "kind") === "dataset" ? "Specific data" : "This workspace";
  if (text(area, "detail")) areaText += `: ${text(area, "detail")}`;
  const rows: [string, string][] = [
    ["Purpose", text(agent, "purpose")],
    ["Engagement", engagement(agent)],
    ["Work area", areaText],
    ["What it may do", skillLabels(agent.skills)],
    ["Model", modelLabel(agent)],
    ["Runs", runtime === "python" ? "On agent-runtime" : runtime === "service" ? "As its own service" : "In your workspace"],
    ["Instructions", text(agent, "manifesto")],
    ["What success looks like", text(agent, "goal")],
    ["Runs on its own", text(agent, "trigger")],
    ["Status", text(agent, "status")],
  ];
  return (
    <>
      <section className="panel">
        <h2>About</h2>
        <dl>
          {rows.map(([label, value]) => (
            <div key={label} className="contents">
              <dt>{label}</dt>
              <dd>{value || "—"}</dd>
            </div>
          ))}
        </dl>
        <p>
          <Button to={ws(`/ask?${qs({ customer_id: customerId, agent: id })}`)}>Talk to this agent in Ask Orama</Button>
        </p>
      </section>
      <Pipelines agent={agent} />
      {runtime === "python" && <Schedules agentId={id} />}
      <Runs agent={agent} />
      {(!runtime || runtime === "native") && <Training agentId={id} />}
      <DeleteAgent agent={agent} />
    </>
  );
}

/** Pipelines as tools (lake#238): the whole list is written back, so Zetta checks every grant is
 * still approved and pins each to its approval. */
function Pipelines({ agent }: { agent: Row }) {
  const ws = useWsPath();
  const scope = useWorkspace();
  const toast = useToast();
  const id = text(agent, "id");
  const projects = useWq<{ projects?: Row[] }>("list_projects");
  const change = useAsync();
  const granted: Row[] = Array.isArray(agent.pipelines) ? agent.pipelines : [];
  const grantedKeys = new Set(granted.map((g) => `${text(g, "project")}/${text(g, "graph")}`));
  // Approved Pipelines, without the Project Agent's check graphs; a failed read offers none.
  const offers: { project: string; graph: string }[] = [];
  for (const project of projects.data?.projects ?? []) {
    if (!project || typeof project !== "object") continue;
    for (const graph of Array.isArray(project.graphs) ? (project.graphs as Row[]) : []) {
      const graphName = text(graph, "name");
      if (graphName.startsWith("Project Agent check") || text(graph, "approval") !== "approved") continue;
      if (!grantedKeys.has(`${text(project, "name")}/${graphName}`)) offers.push({ project: text(project, "name"), graph: graphName });
    }
  }
  const write = (project: string, graph: string, grant: boolean) =>
    change.run(async () => {
      const current = await wq<Row>(scope, "get_agent", { id });
      const grants = (Array.isArray(current.pipelines) ? (current.pipelines as Row[]) : [])
        .filter((g) => !(text(g, "project") === project && text(g, "graph") === graph))
        .map((g) => ({ project: text(g, "project"), graph: text(g, "graph") }));
      if (grant) grants.push({ project, graph });
      await wa(scope, "update_agent", { id, pipelines: grants });
      toast(grant ? `Granted ${graph}` : `Revoked ${graph}`);
    });
  return (
    <section className="panel" id="agent-pipelines">
      <h2>Pipelines</h2>
      <p className="sub">
        An approved Pipeline granted here becomes one of this agent's tools: it checks the correctness gate, runs it and reads the report. The grant keeps the
        approved version.
      </p>
      {change.error ? <ErrorBlock error={change.error} /> : null}
      {granted.length === 0 ? (
        <p>No Pipelines granted.</p>
      ) : (
        <ul>
          {granted.map((grant) => (
            <li key={`${text(grant, "project")}/${text(grant, "graph")}`}>
              <Link to={ws(`/design/pipeline?${qs({ customer_id: scope.customerId, project: text(grant, "project"), graph: text(grant, "graph") })}`)}>
                {text(grant, "graph")}
              </Link>{" "}
              in {text(grant, "project")} <small>approved {text(grant, "version")}</small>{" "}
              <Button disabled={change.busy} onClick={() => void write(text(grant, "project"), text(grant, "graph"), false)}>
                Revoke
              </Button>
            </li>
          ))}
        </ul>
      )}
      {offers.length > 0 && (
        <>
          <h3>Approved Pipelines You Can Grant</h3>
          <ul>
            {offers.map((offer) => (
              <li key={`${offer.project}/${offer.graph}`}>
                {offer.graph} in {offer.project}{" "}
                <Button disabled={change.busy} onClick={() => void write(offer.project, offer.graph, true)}>
                  Grant
                </Button>
              </li>
            ))}
          </ul>
        </>
      )}
    </section>
  );
}

/** "every monday at 09:00" is a trigger in plain words; five fields or an @macro is cron. */
export function isCron(when: string): boolean {
  return when.startsWith("@") || ((when.match(/ /g)?.length ?? 0) >= 4 && /^[0-9*/,\- ]*$/.test(when));
}

function Schedules({ agentId }: { agentId: string }) {
  const scope = useWorkspace();
  const toast = useToast();
  const schedules = useSpaceRead<{ schedules?: Row[] }>("list_agent_schedules", { agent_id: agentId });
  const add = useAsync();
  const remove = useAsync();
  const [when, setWhen] = useState("");
  const [input, setInput] = useState("");
  return (
    <section className="panel" id="agent-schedules">
      <h2>Schedules</h2>
      <p className="sub">Each schedule is its own Kubernetes CronJob: every tick runs this agent once, as you, with what you ask it here.</p>
      {remove.error ? <ErrorBlock error={remove.error} /> : null}
      <DataTable
        q={schedules}
        what="the schedules"
        rows={schedules.data?.schedules}
        empty={<p>No schedules. It runs when you run it.</p>}
        rowKey={(s) => text(s, "id")}
        columns={[
          { key: "when", label: "When", render: (s) => text(s, "description") },
          {
            key: "cron",
            label: "Cron",
            render: (s) => (
              <>
                <code>{text(s, "cron")}</code> <small>{text(s, "time_zone")}</small>
              </>
            ),
          },
          { key: "asked", label: "Asked", render: (s) => (typeof s.input === "string" ? s.input : "") },
          {
            key: "status",
            label: "Status",
            render: (s) => (
              <>
                <span className="badge">{text(s, "phase") || "Pending"}</span> <small>{text(s, "message")}</small>
              </>
            ),
          },
          { key: "last", label: "Last fired", render: (s) => text(s, "last_schedule_at") },
          {
            key: "remove",
            label: "",
            render: (s) => (
              <Button
                disabled={remove.busy}
                onClick={() =>
                  void remove.run(async () => {
                    await act("unschedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, schedule_id: text(s, "id") });
                    toast("Schedule removed");
                  })
                }
              >
                Remove
              </Button>
            ),
          },
        ]}
      />
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void add.run(async () => {
            const body: Row = { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: agentId, [isCron(when) ? "cron" : "trigger"]: when };
            if (input) body.input = input;
            await act("schedule_agent", body);
            setWhen("");
            setInput("");
            toast("Schedule added");
          });
        }}
      >
        {add.error ? <ErrorBlock error={add.error} /> : null}
        <Field label="When">
          <input name="when" required placeholder="every monday at 09:00, or 0 9 * * 1" value={when} onChange={(e) => setWhen(e.target.value)} />
        </Field>
        <Field label="Ask it">
          <input name="input" placeholder="What should each run do?" value={input} onChange={(e) => setInput(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" loading={add.busy}>
            Add schedule
          </Button>
        </div>
      </form>
    </section>
  );
}

function launchStatus(launch: Row): { status: string; live: boolean; starting: boolean } {
  const phase = text(launch, "phase");
  const present = text(launch, "desired") === "present";
  const live = present && phase !== "Succeeded" && phase !== "Failed";
  // A run lake stopped because the operator never started it reads Failed, with why.
  const status = !present && phase !== "Failed" ? "Stopped" : phase || "Pending";
  return { status, live, starting: live && phase !== "Ready" };
}

function Runs({ agent }: { agent: Row }) {
  const scope = useWorkspace();
  const toast = useToast();
  const id = text(agent, "id");
  const python = text(agent, "runtime") === "python";
  // Checks again every 5 seconds while a run is starting (the page's meta refresh).
  const launches = useSpaceRead<{ launches?: Row[] }>(
    "list_agent_launches",
    { agent_id: id, limit: 20 },
    { refetchInterval: (q) => (((q.state.data as { launches?: Row[] } | undefined)?.launches ?? []).some((l) => launchStatus(l).starting) ? 5000 : false) },
  );
  const stop = useAsync();
  const run = useAsync();
  const [input, setInput] = useState("");
  const rows = launches.data?.launches;
  const changing = (rows ?? []).some((l) => launchStatus(l).starting);
  return (
    <section className="panel" id="agent-runs">
      <h2>Unattended Runs</h2>
      {stop.error ? <ErrorBlock error={stop.error} /> : null}
      <DataTable
        q={launches}
        what="the runs"
        rows={rows}
        empty={text(agent, "trigger") ? <p>No unattended runs yet. Its trigger runs it.</p> : <p>This agent only talks: it has no trigger.</p>}
        rowKey={(l) => text(l, "id")}
        columns={[
          { key: "started", label: "Started", render: (l) => text(l, "created_at") },
          { key: "how", label: "How", render: (l) => text(l, "trigger") },
          { key: "mode", label: "Mode", render: (l) => text(l, "mode") },
          {
            key: "status",
            label: "Status",
            render: (l) => (
              <>
                <span className="badge">{launchStatus(l).status}</span> <small>{text(l, "message")}</small>
              </>
            ),
          },
          {
            key: "result",
            label: "Result",
            render: (l) => {
              const result = l.result && typeof l.result === "object" ? (l.result as Row) : null;
              return (result ? text(result, "reply") || text(result, "error") : "").slice(0, 400);
            },
          },
          {
            key: "stop",
            label: "",
            render: (l) =>
              // A scheduled run belongs to its CronJob; removing the schedule stops the next ones.
              launchStatus(l).live && text(l, "trigger") !== "schedule" ? (
                <Button
                  disabled={stop.busy}
                  onClick={() =>
                    void stop.run(async () => {
                      await act("stop_agent_launch", { customer_id: scope.customerId, workspace_id: scope.workspaceId, launch_id: text(l, "id") });
                      toast("Run stopped");
                    })
                  }
                >
                  Stop
                </Button>
              ) : null,
          },
        ]}
      />
      {python && (
        <form
          className="stack"
          onSubmit={(e) => {
            e.preventDefault();
            void run.run(async () => {
              await act("launch_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id, mode: "job", input: input || null });
              setInput("");
              toast("Run started");
            });
          }}
        >
          {run.error ? <ErrorBlock error={run.error} /> : null}
          <Field label="Run it once with">
            <input name="input" placeholder="What should it do this time?" value={input} onChange={(e) => setInput(e.target.value)} />
          </Field>
          <div>
            <Button variant="primary" type="submit" loading={run.busy}>
              Run now
            </Button>
          </div>
        </form>
      )}
      {changing && (
        <p className="sub" role="status">
          A run is starting. This page checks again every 5 seconds.
        </p>
      )}
    </section>
  );
}

function Training({ agentId }: { agentId: string }) {
  const training = useWq<Row>("agent_training_status", { id: agentId });
  // The page showed this panel only when the read answered.
  if (!training.data || !Object.keys(training.data).length) return null;
  return (
    <section className="panel">
      <h2>Training</h2>
      <p>{text(training.data, "reason")}</p>
    </section>
  );
}

function DeleteAgent({ agent }: { agent: Row }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const name = text(agent, "name");
  const id = text(agent, "id");
  const [confirm, setConfirm] = useState("");
  const remove = useAsync();
  return (
    <section className="panel">
      <h2>Delete Agent</h2>
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void remove.run(async () => {
            if (confirm !== name) throw new Error("Type the agent's name to delete it.");
            await wa(scope, "delete_agent", { id });
            // No CronJob outlives its agent.
            try {
              const listed = await read<{ schedules?: Row[] }>("list_agent_schedules", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id }).catch(() => ({ schedules: [] as Row[] }));
              for (const schedule of listed.schedules ?? []) {
                await act("unschedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, schedule_id: text(schedule, "id") }).catch(() => undefined);
              }
            } finally {
              toast(`Deleted ${name}`);
              navigate(ws(`/train/agents?${qs({ customer_id: scope.customerId })}`));
            }
          });
        }}
      >
        {remove.error ? <ErrorBlock error={remove.error} /> : null}
        <Field
          label={
            <>
              Type <strong>{name}</strong> to delete this agent
            </>
          }
        >
          <input name="confirm" required autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </Field>
        <div>
          <Button variant="danger" type="submit" loading={remove.busy}>
            Delete agent
          </Button>
        </div>
      </form>
    </section>
  );
}
