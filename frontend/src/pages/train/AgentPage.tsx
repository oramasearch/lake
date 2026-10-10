/** Train → Agent (web_ui.cpp agent_content): one agent, with Overview (what it is for and its
 * pipelines as tools), its unattended runs, its schedules and its training as tabs, and its facts
 * and actions beside them. Each control calls the capability its form handler called
 * (http_server.cpp agent_grant … agent_delete). */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Bot, CalendarClock, MessageSquare, Play, Plus, Trash2, Workflow } from "lucide-react";
import { ApiError, act, read, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, KeyValue, Mono, Skeleton, Split, StateTabs, Status } from "@/lib/ui";
import { AGENT_SKILLS, engagement, modelLabel, runsWhere } from "./agents";
import { WorkspaceFrame, qs, text, timeAgo, useAsync, useSpaceRead, useWorkspace, useWq, useWsPath, wa, wq } from "./workspace";

export default function AgentPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  const query = qs({ customer_id: customerId });
  const agent = useWq<Row>(id ? "get_agent" : null, { id });
  const name = agent.data ? text(agent.data, "name") : "";
  const found = !!agent.data && Object.keys(agent.data).length > 0;
  useTrail(
    [{ label: "Train", to: ws(`/train/agents?${query}`) }, { label: "Agents", to: ws(`/train/agents?${query}`) }, entityCrumb(id || "Agent")],
    `Agent: ${name || id}`,
  );
  const missing = (
    <Empty icon={Bot} title="No such agent in this workspace" action={<Button to={ws(`/train/agents?${query}`)}>All agents</Button>}>
      It may have been deleted, or the link names another workspace.
    </Empty>
  );
  return (
    <WorkspaceFrame
      eyebrow="Train"
      title={
        name ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {name}
            <Status value={text(agent.data, "status") || "idle"} />
          </span>
        ) : (
          `Agent: ${id}`
        )
      }
      sub={found ? text(agent.data, "purpose") || undefined : undefined}
      actions={
        found ? (
          <Button variant="primary" to={ws(`/ask?${qs({ customer_id: customerId, agent: id })}`)}>
            <MessageSquare size={14} aria-hidden /> Talk to this agent in Ask Orama
          </Button>
        ) : null
      }
    >
      {() => {
        if (!id) return missing;
        if (agent.isLoading) return <Skeleton />;
        if (agent.isError) {
          if (agent.error instanceof ApiError && agent.error.status === 404) return missing;
          return <ErrorBlock error={agent.error} retry={() => void agent.refetch()} what="the agent" />;
        }
        if (!found) return missing;
        return <AgentContent agent={agent.data!} />;
      }}
    </WorkspaceFrame>
  );
}

type Tab = "overview" | "runs" | "schedules" | "training";

function AgentContent({ agent }: { agent: Row }) {
  const id = text(agent, "id");
  const runtime = text(agent, "runtime");
  const native = !runtime || runtime === "native";
  const [tab, setTab] = useState<Tab>("overview");
  const [deleting, setDeleting] = useState(false);
  // The page showed Training only when the read answered.
  const training = useWq<Row>(native ? "agent_training_status" : null, { id });
  const trained = !!training.data && Object.keys(training.data).length > 0;
  const area = (agent.work_area && typeof agent.work_area === "object" ? agent.work_area : {}) as Row;
  let areaText = text(area, "kind") === "project" ? "A single project" : text(area, "kind") === "dataset" ? "Specific data" : "This workspace";
  if (text(area, "detail")) areaText += `: ${text(area, "detail")}`;
  const skills = (Array.isArray(agent.skills) ? agent.skills : []).filter((s): s is string => typeof s === "string");
  const tabs: { value: Tab; label: string }[] = [
    { value: "overview", label: "Overview" },
    { value: "runs", label: "Unattended Runs" },
    ...(runtime === "python" ? [{ value: "schedules" as const, label: "Schedules" }] : []),
    ...(trained ? [{ value: "training" as const, label: "Training" }] : []),
  ];
  return (
    <>
      <Split
        side={
          <>
            <Card label="Facts">
              <KeyValue
                rows={[
                  ["Engagement", engagement(agent)],
                  ["Work area", areaText],
                  [
                    "What it may do",
                    skills.length ? (
                      <span className="flex flex-wrap gap-1">
                        {skills.map((s) => (
                          <Chip key={s}>{AGENT_SKILLS.find(([key]) => key === s)?.[1] ?? s}</Chip>
                        ))}
                      </span>
                    ) : null,
                  ],
                  ["Model", modelLabel(agent) ? <Mono>{modelLabel(agent)}</Mono> : null],
                  ["Runs", runsWhere(agent)],
                  ["Runs on its own", text(agent, "trigger")],
                  ["Status", <Status key="status" value={text(agent, "status") || "idle"} />],
                  ["Id", <Mono key="id" short={12}>{id}</Mono>],
                ]}
              />
            </Card>
            <Card label="Actions">
              <div className="flex flex-col gap-2">
                <AskButton id={id} />
                <Button variant="danger" onClick={() => setDeleting(true)}>
                  <Trash2 size={14} aria-hidden /> Delete agent
                </Button>
                <p className="muted m-0 text-[12px]">Deleting the agent also removes its schedules.</p>
              </div>
            </Card>
          </>
        }
      >
        <StateTabs label="Agent" value={tab} onChange={setTab} tabs={tabs} />
        {tab === "overview" && (
          <>
            <Card label="About">
              <KeyValue
                rows={[
                  ["Instructions", text(agent, "manifesto") ? <span className="whitespace-pre-wrap">{text(agent, "manifesto")}</span> : null],
                  ["What success looks like", text(agent, "goal")],
                ]}
              />
            </Card>
            <Pipelines agent={agent} />
          </>
        )}
        {tab === "runs" && <Runs agent={agent} />}
        {tab === "schedules" && runtime === "python" && <Schedules agentId={id} />}
        {tab === "training" && trained && (
          <Card label="Training">
            <p className="m-0">{text(training.data, "reason")}</p>
          </Card>
        )}
      </Split>
      {deleting && <DeleteAgent agent={agent} onClose={() => setDeleting(false)} />}
    </>
  );
}

function AskButton({ id }: { id: string }) {
  const ws = useWsPath();
  const { customerId } = useWorkspace();
  return (
    <Button to={ws(`/ask?${qs({ customer_id: customerId, agent: id })}`)}>
      <MessageSquare size={14} aria-hidden /> Talk in Ask Orama
    </Button>
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
    <Card label="Pipelines" title="Tools This Agent May Use" className="scroll-mt-4">
      <p className="muted mt-0 text-[12.5px]">
        An approved Pipeline granted here becomes one of this agent's tools: it checks the correctness gate, runs it and reads the report. The grant keeps the
        approved version.
      </p>
      {change.error ? <ErrorBlock error={change.error} /> : null}
      {granted.length === 0 ? (
        <Empty compact icon={Workflow} title="No Pipelines granted">
          {offers.length ? "Grant an approved Pipeline below." : "Approve a Pipeline in a project to grant it here."}
        </Empty>
      ) : (
        <ul className="m-0 list-none divide-y divide-hairline p-0">
          {granted.map((grant) => (
            <li key={`${text(grant, "project")}/${text(grant, "graph")}`} className="flex flex-wrap items-center gap-2 py-2">
              <span className="min-w-0 flex-1">
                <Link to={ws(`/design/pipeline?${qs({ customer_id: scope.customerId, project: text(grant, "project"), graph: text(grant, "graph") })}`)} className="font-semibold">
                  {text(grant, "graph")}
                </Link>{" "}
                <span className="muted">in {text(grant, "project")}</span>
              </span>
              <Chip tone="ok">approved {text(grant, "version")}</Chip>
              <Button variant="ghost" disabled={change.busy} onClick={() => void write(text(grant, "project"), text(grant, "graph"), false)}>
                Revoke
              </Button>
            </li>
          ))}
        </ul>
      )}
      {offers.length > 0 && (
        <div className="mt-4 border-t border-hairline pt-3">
          <div className="label m-0">Approved Pipelines You Can Grant</div>
          <ul className="m-0 list-none divide-y divide-hairline p-0">
            {offers.map((offer) => (
              <li key={`${offer.project}/${offer.graph}`} className="flex flex-wrap items-center gap-2 py-2">
                <span className="min-w-0 flex-1">
                  <span className="font-semibold text-ink">{offer.graph}</span> <span className="muted">in {offer.project}</span>
                </span>
                <Button disabled={change.busy} onClick={() => void write(offer.project, offer.graph, true)}>
                  <Plus size={14} aria-hidden /> Grant
                </Button>
              </li>
            ))}
          </ul>
        </div>
      )}
    </Card>
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
  const [adding, setAdding] = useState(false);
  const [when, setWhen] = useState("");
  const [input, setInput] = useState("");
  return (
    <Card
      label="Schedules"
      title="When It Runs on Its Own"
      actions={
        <Button onClick={() => setAdding(true)}>
          <Plus size={14} aria-hidden /> Add schedule
        </Button>
      }
    >
      <p className="muted mt-0 text-[12.5px]">Each schedule is its own Kubernetes CronJob: every tick runs this agent once, as you, with what you ask it here.</p>
      {remove.error ? <ErrorBlock error={remove.error} /> : null}
      <DataTable
        q={schedules}
        what="the schedules"
        rows={schedules.data?.schedules}
        empty={
          <Empty compact icon={CalendarClock} title="No schedules" action={<Button onClick={() => setAdding(true)}>Add schedule</Button>}>
            It runs when you run it.
          </Empty>
        }
        rowKey={(s) => text(s, "id")}
        columns={[
          {
            key: "when",
            label: "When",
            render: (s) => (
              <div className="min-w-0">
                <div className="font-semibold text-ink">{text(s, "description")}</div>
                <div className="font-mono text-[11.5px] text-ink-3">
                  {text(s, "cron")} {text(s, "time_zone")}
                </div>
              </div>
            ),
          },
          { key: "asked", label: "Asked", render: (s) => (typeof s.input === "string" ? s.input : "") },
          {
            key: "status",
            label: "Status",
            render: (s) => (
              <div className="min-w-0">
                <Status value={text(s, "phase") || "Pending"} />
                {text(s, "message") && <div className="mt-0.5 text-[12px] text-ink-3">{text(s, "message")}</div>}
              </div>
            ),
          },
          {
            key: "last",
            label: "Last fired",
            render: (s) => (
              <span className="font-mono text-[12px] text-ink-2" title={text(s, "last_schedule_at")}>
                {text(s, "last_schedule_at") ? timeAgo(text(s, "last_schedule_at")) : "—"}
              </span>
            ),
          },
          {
            key: "remove",
            label: "",
            className: "text-right",
            render: (s) => (
              <Button
                variant="ghost"
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
      {adding && (
        <Modal
          title="Add a Schedule"
          subtitle="Every tick runs this agent once, as you."
          onClose={() => setAdding(false)}
          footer={
            <>
              <Button onClick={() => setAdding(false)}>Cancel</Button>
              <Button variant="primary" type="submit" form="add-schedule" loading={add.busy}>
                Add schedule
              </Button>
            </>
          }
        >
          <form
            id="add-schedule"
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
                setAdding(false);
              });
            }}
          >
            {add.error ? <ErrorBlock error={add.error} /> : null}
            <Field label="When" help="In words, or five cron fields.">
              <input name="when" required placeholder="every monday at 09:00, or 0 9 * * 1" value={when} onChange={(e) => setWhen(e.target.value)} />
            </Field>
            <Field label="Ask it">
              <input name="input" placeholder="What should each run do?" value={input} onChange={(e) => setInput(e.target.value)} />
            </Field>
          </form>
        </Modal>
      )}
    </Card>
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
    <Card label="Unattended Runs" title="What It Did on Its Own">
      {python && (
        <form
          className="mb-3 flex flex-wrap items-end gap-2"
          onSubmit={(e) => {
            e.preventDefault();
            void run.run(async () => {
              await act("launch_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id, mode: "job", input: input || null });
              setInput("");
              toast("Run started");
            });
          }}
        >
          <label className="flex min-w-0 flex-1 flex-col gap-1 text-[12.5px] text-ink-2">
            Run it once with
            <input name="input" placeholder="What should it do this time?" value={input} onChange={(e) => setInput(e.target.value)} />
          </label>
          <Button variant="primary" type="submit" loading={run.busy}>
            <Play size={14} aria-hidden /> Run now
          </Button>
        </form>
      )}
      {run.error ? <ErrorBlock error={run.error} /> : null}
      {stop.error ? <ErrorBlock error={stop.error} /> : null}
      {changing && <Banner tone="quiet">A run is starting. This page checks again every 5 seconds.</Banner>}
      <DataTable
        q={launches}
        what="the runs"
        rows={rows}
        empty={
          <Empty compact icon={Play} title={text(agent, "trigger") ? "No unattended runs yet" : "This agent only talks"}>
            {text(agent, "trigger") ? "Its trigger runs it." : "It has no trigger."}
          </Empty>
        }
        rowKey={(l) => text(l, "id")}
        columns={[
          {
            key: "started",
            label: "Started",
            render: (l) => (
              <span className="font-mono text-[12px] text-ink-2" title={text(l, "created_at")}>
                {timeAgo(text(l, "created_at"))}
              </span>
            ),
          },
          {
            key: "how",
            label: "How",
            render: (l) => (
              <span className="flex flex-wrap gap-1">
                {text(l, "trigger") && <Chip>{text(l, "trigger")}</Chip>}
                {text(l, "mode") && <Chip>{text(l, "mode")}</Chip>}
              </span>
            ),
          },
          {
            key: "status",
            label: "Status",
            render: (l) => (
              <div className="min-w-0">
                <Status value={launchStatus(l).status} />
                {text(l, "message") && <div className="mt-0.5 text-[12px] text-ink-3">{text(l, "message")}</div>}
              </div>
            ),
          },
          {
            key: "result",
            label: "Result",
            render: (l) => {
              const result = l.result && typeof l.result === "object" ? (l.result as Row) : null;
              return <span className="text-[12.5px] text-ink-2">{(result ? text(result, "reply") || text(result, "error") : "").slice(0, 400)}</span>;
            },
          },
          {
            key: "stop",
            label: "",
            className: "text-right",
            render: (l) =>
              // A scheduled run belongs to its CronJob; removing the schedule stops the next ones.
              launchStatus(l).live && text(l, "trigger") !== "schedule" ? (
                <Button
                  variant="ghost"
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
    </Card>
  );
}

function DeleteAgent({ agent, onClose }: { agent: Row; onClose: () => void }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const name = text(agent, "name");
  const id = text(agent, "id");
  const [confirm, setConfirm] = useState("");
  const remove = useAsync();
  return (
    <Modal
      title={`Delete ${name}?`}
      onClose={onClose}
      width={460}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="danger" type="submit" form="delete-agent" loading={remove.busy} disabled={confirm !== name}>
            Delete agent
          </Button>
        </>
      }
    >
      <Banner tone="warn">The agent is deleted, and its schedules with it.</Banner>
      <form
        id="delete-agent"
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
      </form>
    </Modal>
  );
}
