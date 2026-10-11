/** Train → Agent (lake#653): one agent's training page, and where it runs. A trainable agent (its
 * behaviour comes from a model, a pipeline or a training schedule) opens on Training: its
 * performance over time (held-out scores, task success rate, latency, cost), its trained
 * versions and the data each learned from, its training schedule, Launch experiment through
 * ExperimentsAgent, and promote and roll back. Every agent has its unattended runs (Run now) and,
 * for a frontier-model agent, its schedules. Designing it is Design → Agent, linked from here. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Bot, CalendarClock, FlaskConical, GraduationCap, MessageSquare, PenLine, Play, Plus, Workflow } from "lucide-react";
import { ApiError, act, type Row } from "@/lib/api";
import { ConfirmButton, Drawer, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { entityCrumb, useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, Form, FormError, KeyValue, Mono, Skeleton, Split, Stat, StatRow, StateTabs, Status } from "@/lib/ui";
import {
  CADENCES,
  bestVersion,
  currentVersion,
  experimentAsk,
  isNative,
  modelLabel,
  pipelinesOf,
  runFigures,
  runsWhere,
  scoreSeries,
  switchInput,
  switchKind,
  trainable,
  trainedVersions,
  trainingInput,
  type ExperimentAxis,
  type TrainedVersion,
} from "@/lib/agents";
import { Chart } from "@/pages/experiments/Chart";
import { engagement, useEngineRead } from "./agents";
import { WorkspaceFrame, qs, text, timeAgo, useAsync, useSpaceRead, useWorkspace, useWsPath } from "./workspace";
import { UsageThisMonth } from "../usage/UsageThisMonth";

export default function AgentPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const ws = useWsPath();
  const agent = useEngineRead<Row>(id ? "get_agent" : null, { id }, { retry: false });
  const name = agent.data ? text(agent.data, "name") : "";
  const found = !!agent.data && !!text(agent.data, "id");
  useTrail([{ label: "Train", to: ws("/train/agents") }, { label: "Agents", to: ws("/train/agents") }, entityCrumb(name || id || "Agent")], `Agent: ${name || id}`);
  const missing = (
    <Empty icon={Bot} title="No such agent in this workspace" action={<Button to={ws("/train/agents")}>All agents</Button>}>
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
          <>
            <Button to={ws(`/design/agent?${qs({ id })}`)}>
              <PenLine size={14} aria-hidden /> Edit its design
            </Button>
            <Button variant="primary" to={`/?${qs({ agent: id })}`}>
              <MessageSquare size={14} aria-hidden /> Talk in Ask Orama
            </Button>
          </>
        ) : null
      }
    >
      {() => {
        if (!id) return missing;
        if (agent.isLoading) return <Skeleton />;
        if (agent.isError) {
          if (agent.error instanceof ApiError && (agent.error.status === 404 || agent.error.code === "not_found")) return missing;
          return <ErrorBlock error={agent.error} retry={() => void agent.refetch()} what="the agent" />;
        }
        if (!found) return missing;
        return <AgentContent agent={agent.data!} />;
      }}
    </WorkspaceFrame>
  );
}

type Tab = "training" | "runs" | "schedules";

function AgentContent({ agent }: { agent: Row }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const [params, setParams] = useSearchParams();
  const id = text(agent, "id");
  const python = text(agent, "runtime") === "python";
  const canTrain = trainable(agent);
  const tabs: { value: Tab; label: string }[] = [
    ...(canTrain ? [{ value: "training" as const, label: "Training" }] : []),
    { value: "runs", label: "Unattended Runs" },
    ...(python ? [{ value: "schedules" as const, label: "Schedules" }] : []),
  ];
  const asked = params.get("tab") as Tab | null;
  const tab: Tab = asked && tabs.some((t) => t.value === asked) ? asked : tabs[0].value;
  const setTab = (next: Tab) => {
    const copy = new URLSearchParams(params);
    copy.set("tab", next);
    setParams(copy, { replace: true });
  };
  const area = (agent.work_area && typeof agent.work_area === "object" ? agent.work_area : {}) as Row;
  let areaText = text(area, "kind") === "project" ? "A single project" : text(area, "kind") === "dataset" ? "Specific data" : "This workspace";
  if (text(area, "detail")) areaText += `: ${text(area, "detail")}`;
  return (
    <Split
      side={
        <>
          <Card label="Facts">
            <KeyValue
              rows={[
                ["Engagement", engagement(agent)],
                ["Model", modelLabel(agent) ? <Mono key="m">{modelLabel(agent)}</Mono> : null],
                ["Runs", runsWhere(agent)],
                ["Work area", areaText],
                ["Runs on its own", text(agent, "trigger")],
                ["Id", <Mono key="id" short={12}>{id}</Mono>],
              ]}
            />
          </Card>
          <Card label="Design">
            <p className="muted mt-0 text-[12.5px]">Its purpose, prompt, skills, model and pipelines are designed in Design → Agents, where every revision is kept.</p>
            <Button to={ws(`/design/agent?${qs({ id })}`)}>
              <PenLine size={14} aria-hidden /> Edit its design
            </Button>
          </Card>
          <UsageThisMonth
            customerId={scope.customerId}
            workspaceId={scope.workspaceId}
            match={[{ dimension: "model_tokens", breakdown: "by_agent", keys: [text(agent, "name"), id], label: "Model tokens" }]}
            note="Its cost: counted when the agent reports the tokens it spends."
          />
        </>
      }
    >
      <StateTabs label="Agent" value={tab} onChange={setTab} tabs={tabs} />
      {!canTrain && (
        <Banner tone="quiet" title="Nothing to Train">
          This agent runs on a frontier model and calls no pipeline, so Lake has nothing to train. Give it the Orama base model or a pipeline in its design to train it here.
        </Banner>
      )}
      {tab === "training" && canTrain && <Training agent={agent} />}
      {tab === "runs" && <Runs agent={agent} />}
      {tab === "schedules" && python && <Schedules agentId={id} />}
    </Split>
  );
}

/* ------------------------------------------------------------------ training */

function Training({ agent }: { agent: Row }) {
  const ws = useWsPath();
  const id = text(agent, "id");
  const native = isNative(agent);
  const status = useEngineRead<Row>(native ? "agent_training_status" : null, { id }, { retry: false });
  const launches = useSpaceRead<{ launches?: Row[] }>("list_agent_launches", { agent_id: id, limit: 50 });
  const versions = trainedVersions(status.data);
  const current = currentVersion(agent, versions);
  const best = bestVersion(versions);
  const runs = runFigures(launches.data?.launches ?? []);
  const [xs, ys] = scoreSeries(versions);
  const pipelines = pipelinesOf(agent);
  return (
    <>
      <StatRow>
        <Stat label="Held-out score" icon={GraduationCap} value={current?.eval_score ?? best?.eval_score ?? "—"} sub={current ? "of the version it runs" : best ? "best version" : "not evaluated yet"} />
        <Stat label="Versions" value={versions.length} sub={status.data?.stale ? "stale: retrain it" : status.data?.due ? "a retrain is due" : "trained"} tone={status.data?.stale ? "bad" : undefined} />
        <Stat label="Task success" value={runs.successRate === null ? "—" : `${Math.round(runs.successRate * 100)}%`} sub={runs.finished ? `${runs.succeeded} of ${runs.finished} recent runs` : "no finished runs yet"} />
        <Stat label="Latency" value={runs.medianSeconds === null ? "—" : `${Math.round(runs.medianSeconds)} s`} sub="median run, start to finish" />
      </StatRow>
      {status.isError ? <ErrorBlock error={status.error} retry={() => void status.refetch()} what="its training" /> : null}
      {status.data?.reason ? (
        <Banner tone={status.data.stale ? "warn" : "quiet"} title={status.data.stale ? "Stale" : status.data.due ? "Retrain Due" : "Training"}>
          {text(status.data, "reason")}
        </Banner>
      ) : null}
      {Array.isArray(status.data?.unverifiable_goals) && status.data!.unverifiable_goals.length > 0 && (
        <Banner tone="warn" title="Goals No Check Can Verify">
          Training drops these goals, since nothing can score them: {(status.data!.unverifiable_goals as unknown[]).map((g) => (typeof g === "string" ? g : text(g as Row, "statement"))).join("; ")}
        </Banner>
      )}
      {native && (
        <Card label="Performance" title="Held-Out Score Over Time">
          {xs.length >= 1 ? (
            <Chart chartKey={`agent-${id}-score`} data={[xs, ys]} labels={["held-out score"]} settings={{ time: true, metric: true }} />
          ) : (
            <Empty compact icon={GraduationCap} title="No Evaluated Version Yet">
              Each trained version's held-out score is plotted here once it is evaluated.
            </Empty>
          )}
        </Card>
      )}
      {native && <Versions agent={agent} versions={versions} current={current} best={best} loading={status.isLoading} />}
      {pipelines.length > 0 && (
        <Card label="Pipelines" title="The Pipelines It Calls">
          <p className="muted mt-0 text-[12.5px]">Its answers come from these approved pipelines. Train and compare their runs to improve it.</p>
          <ul className="m-0 list-none divide-y divide-hairline p-0">
            {pipelines.map((p) => (
              <li key={`${p.project}/${p.graph}`} className="flex flex-wrap items-center gap-2 py-2">
                <Workflow size={14} className="text-ink-3" aria-hidden />
                <Link to={ws(`/design/pipeline?${qs({ project: p.project, graph: p.graph })}`)} className="font-semibold">
                  {p.graph}
                </Link>
                <span className="muted">in {p.project}</span>
                <Link to={ws(`/train/runs?${qs({ project: p.project })}`)} className="ml-auto text-[12.5px]">
                  Its runs
                </Link>
              </li>
            ))}
          </ul>
        </Card>
      )}
      <Experiments agent={agent} versions={versions} />
      {native && <TrainingSchedule agent={agent} />}
    </>
  );
}

/** Every version training produced, with the data it learned from, its score, and Promote or
 * Roll back (the agent's model becomes that version's checkpoint, through update_agent). */
function Versions({ agent, versions, current, best, loading }: { agent: Row; versions: TrainedVersion[]; current: TrainedVersion | null; best: TrainedVersion | null; loading: boolean }) {
  const toast = useToast();
  const scope = useWorkspace();
  const id = text(agent, "id");
  const rows = [...versions].reverse();
  return (
    <Card label="Training Runs" title="Versions and the Data They Learned From">
      {loading ? <Skeleton /> : <DataTable
        what="the versions"
        rows={rows}
        empty={
          <Empty compact icon={GraduationCap} title="Not Trained Yet">
            Set a training schedule below, or run an experiment, and each version it trains is listed here.
          </Empty>
        }
        rowKey={(v) => v.checkpoint || v.created_at}
        columns={[
          { key: "when", label: "Trained", render: (v) => <span title={v.created_at}>{timeAgo(v.created_at)}</span> },
          {
            key: "version",
            label: "Version",
            render: (v) => (
              <span className="inline-flex flex-wrap items-center gap-1">
                <Mono short={24}>{v.checkpoint}</Mono>
                {current?.checkpoint === v.checkpoint && <Chip tone="ok">running</Chip>}
                {best?.checkpoint === v.checkpoint && <Chip tone="action">best</Chip>}
              </span>
            ),
          },
          { key: "data", label: "Data", render: (v) => v.corpus || <span className="muted">built-in corpus</span> },
          { key: "score", label: "Held-out", numeric: true, render: (v) => (v.eval_score === null ? <span className="muted">—</span> : <span className="font-mono">{v.eval_score.toFixed(3)}</span>) },
          { key: "job", label: "Job", render: (v) => (v.job ? <Mono short={10}>{v.job}</Mono> : <span className="muted">—</span>) },
          {
            key: "switch",
            label: "",
            className: "text-right",
            render: (v) => {
              const kind = switchKind(v, current);
              if (!kind) return null;
              return (
                <ConfirmButton
                  variant={kind === "promote" ? "secondary" : "ghost"}
                  title={kind === "promote" ? "Promote This Version?" : "Roll Back to This Version?"}
                  confirmLabel={kind === "promote" ? "Promote" : "Roll back"}
                  confirm={
                    <>
                      {text(agent, "name")} will run on <code>{v.checkpoint}</code>
                      {v.eval_score !== null ? ` (held-out ${v.eval_score.toFixed(3)})` : ""}. Its design keeps a revision of the change.
                    </>
                  }
                  onConfirm={async () => {
                    await act("update_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...switchInput(id, v) });
                    toast(kind === "promote" ? "Version promoted" : "Rolled back");
                  }}
                >
                  {kind === "promote" ? "Promote" : "Roll back"}
                </ConfirmButton>
              );
            },
          },
        ]}
      />}
    </Card>
  );
}

/** The training spec (set_training_schedule): what it learns from, what it starts from, how many
 * steps, and when it retrains on its own. Manual never retrains behind your back. */
function TrainingSchedule({ agent }: { agent: Row }) {
  const scope = useWorkspace();
  const toast = useToast();
  const spec = agent.training && typeof agent.training === "object" ? (agent.training as Row) : {};
  const [form, setForm] = useState(() => ({
    cadence: text(spec, "cadence") || "manual",
    corpus: text(spec, "corpus"),
    base_checkpoint: text(spec, "base_checkpoint"),
    steps: spec.steps === undefined || spec.steps === null ? "" : String(spec.steps),
  }));
  const save = useAsync();
  return (
    <Card label="Training Schedule" title="When and What It Retrains On">
      <Form
        onSubmit={() =>
          save.run(async () => {
            await act("set_training_schedule", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(agent, "id"), training: trainingInput(form) });
            toast("Training schedule saved");
          })
        }
      >
        <FormError error={save.error} />
        <div className="grid grid-cols-1 gap-x-3 sm:grid-cols-2">
          <Field label="Retrains">
            <select name="cadence" value={form.cadence} onChange={(e) => setForm({ ...form, cadence: e.target.value })}>
              {CADENCES.map((c) => (
                <option key={c.value} value={c.value}>
                  {c.label}
                </option>
              ))}
            </select>
          </Field>
          <Field label="Steps" help="0 or empty: one epoch.">
            <input name="steps" type="number" min={0} value={form.steps} onChange={(e) => setForm({ ...form, steps: e.target.value })} />
          </Field>
        </div>
        <Field label="Training data" help="A corpus id or path; empty uses the built-in corpus.">
          <input name="corpus" value={form.corpus} onChange={(e) => setForm({ ...form, corpus: e.target.value })} />
        </Field>
        <Field label="Start from" help="A checkpoint; empty starts from the model it runs now.">
          <input name="base_checkpoint" value={form.base_checkpoint} onChange={(e) => setForm({ ...form, base_checkpoint: e.target.value })} />
        </Field>
        <div>
          <Button variant="primary" type="submit" loading={save.busy}>
            Save schedule
          </Button>
        </div>
      </Form>
    </Card>
  );
}

/** The experiments run for this agent, and Launch experiment: ExperimentsAgent turns what to
 * compare (versions, prompts or models) into an editable proposal on Train → Experiments, and
 * nothing starts until it is confirmed there. */
function Experiments({ agent, versions }: { agent: Row; versions: TrainedVersion[] }) {
  const ws = useWsPath();
  const id = text(agent, "id");
  const [launching, setLaunching] = useState(false);
  const listed = useSpaceRead<{ experiments?: Row[] }>("list_experiments", {});
  const mine = (listed.data?.experiments ?? []).filter((e) => {
    const spec = (e.spec && typeof e.spec === "object" ? e.spec : {}) as Row;
    return text(spec, "agent_id") === id || text(spec, "agent") === id || (!!text(agent, "name") && text(spec, "name").includes(text(agent, "name")));
  });
  return (
    <Card
      label="Experiments"
      title="Improve It"
      actions={
        <Button variant="primary" onClick={() => setLaunching(true)}>
          <FlaskConical size={14} aria-hidden /> Launch experiment
        </Button>
      }
    >
      <p className="muted mt-0 text-[12.5px]">An experiment compares its versions, prompts or models on held-out data and proposes which to promote. ExperimentsAgent designs it; you confirm it before it starts.</p>
      {listed.isError ? (
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="the experiments" />
      ) : mine.length ? (
        <ul className="m-0 list-none divide-y divide-hairline p-0">
          {mine.map((e) => (
            <li key={text(e, "id")} className="flex flex-wrap items-center gap-2 py-2">
              <Link to={ws(`/train/experiment?${qs({ experiment: text(e, "id") })}`)} className="font-semibold">
                {text(e.spec, "name") || text(e, "id")}
              </Link>
              <Status value={text(e, "status")} />
            </li>
          ))}
        </ul>
      ) : (
        <Empty compact icon={FlaskConical} title="No Experiment for This Agent Yet">
          Launch one to compare what it could run on.
        </Empty>
      )}
      {launching && <LaunchExperiment agent={agent} versions={versions} onClose={() => setLaunching(false)} />}
    </Card>
  );
}

function LaunchExperiment({ agent, versions, onClose }: { agent: Row; versions: TrainedVersion[]; onClose: () => void }) {
  const scope = useWorkspace();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [axis, setAxis] = useState<ExperimentAxis>(versions.length > 1 ? "versions" : "prompts");
  const [goal, setGoal] = useState("");
  const send = useAsync();
  const message = experimentAsk(agent, axis, goal, versions);
  return (
    <Drawer
      title="Launch an Experiment"
      subtitle={text(agent, "name")}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="launch-experiment" loading={send.busy}>
            Ask ExperimentsAgent
          </Button>
        </>
      }
    >
      <Form
        id="launch-experiment"
        onSubmit={() =>
          send.run(async () => {
            const turn = await act<Row>("agent_turn", {
              customer_id: scope.customerId,
              workspace_id: scope.workspaceId,
              agent: "agent-experiments",
              kind: "experiment",
              message,
              context: { agent_id: text(agent, "id"), agent_name: text(agent, "name"), compare: axis },
            });
            onClose();
            navigate(ws(`/train/experiments?${qs({ task: text(turn, "task_id") })}`));
          })
        }
      >
        <FormError error={send.error} />
        <Field label="Compare">
          <select name="axis" value={axis} onChange={(e) => setAxis(e.target.value as ExperimentAxis)}>
            <option value="versions">Its trained versions</option>
            <option value="prompts">Variations of its instructions</option>
            <option value="models">The models it could run on</option>
          </select>
        </Field>
        <Field label="What it should improve" help="Optional. Its goal is used when this is empty.">
          <textarea name="goal" rows={2} value={goal} onChange={(e) => setGoal(e.target.value)} />
        </Field>
        <Field label="ExperimentsAgent is asked">
          <p className="m-0 rounded-[6px] border border-hairline bg-raised/40 p-2 text-[12.5px] text-ink-2">{message}</p>
        </Field>
        <p className="muted m-0 text-[12.5px]">It answers with an editable proposal on Train → Experiments. Nothing starts until you confirm it there.</p>
      </Form>
    </Drawer>
  );
}

/* ------------------------------------------------------------------ runs and schedules */

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
      <p className="muted mt-0 text-[12.5px]">Every tick of a schedule runs this agent once, as you, with what you ask it here.</p>
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
