// SPDX-License-Identifier: MIT

/** Train → Experiments (formerly web_ui_experiments.cpp list mode, serve_experiments_agent_form and
 * the start and compare scripts): the figures, Experiments Agent's turn (asked from a drawer) with
 * its editable proposal and confirmed start, the filter, Start an Experiment in a drawer, the
 * experiments as rich rows with Stop, and the tracked runs to compare. An experiment is the
 * engine's study; lake's capabilities say experiment (lake#561). */
import { type FormEvent, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Bot, FlaskConical, GitCompare, Loader2, Play } from "lucide-react";
import { ApiError, type Row } from "@/lib/api";
import { useQuery } from "@tanstack/react-query";
import { relative } from "@/lib/format";
import { Drawer } from "@/lib/overlay";
import { Banner, Button, Card, type Column, DataTable, Empty, ErrorBlock, FilterChips, Notice, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import {
  AgentEventsLink,
  type AgentTask,
  canonicalJson,
  fetchAgentTask,
  Markdown,
  needsReply,
  taskReply,
  taskResult,
  taskWorking,
  useAgentDeployed,
  useAgentTask,
  useSpacePermissions,
} from "@/pages/apps/shared";
import { useConfirm } from "@/pages/streams/common";
import { describe, ExperimentsShell, useExperimentActions, useExperimentClient, useExperimentUrl } from "./common";
import { cell, METRIC_NAMES, metricLabel } from "./transforms";
import { useWq } from "@/pages/train/workspace";
import { isWorkspaceMethodology, minimized, needsTargetColumn, objectiveMetrics, parameterRange, searchedParameters, studyMethodologies } from "./workspaceMethodology";

const STATES = ["", "pending", "running", "succeeded", "failed", "stopped", "complete"];

type Experiment = { id: string; spec: Row; status: string; trials: Row[]; spent_cost?: unknown };

/** An experiment's figures: trials by state, and the best completed objective in its direction. */
function experimentFigures(experiment: Experiment) {
  const spec = experiment.spec ?? {};
  const objective = (spec.objective ?? {}) as Row;
  const counts: Record<string, number> = {};
  let best: number | null = null;
  for (const trial of experiment.trials ?? []) {
    counts[trial.state] = (counts[trial.state] ?? 0) + 1;
    if (trial.state === "complete" && typeof trial.objective === "number") {
      if (best === null || (objective.direction === "maximize" ? trial.objective > best : trial.objective < best)) best = trial.objective;
    }
  }
  return { counts, best, objective, budgets: (spec.budgets ?? {}) as Row };
}

export default function ExperimentsPage() {
  const [params] = useSearchParams();
  const project = params.get("project") ?? "";
  const state = params.get("state") ?? "";
  const taskId = params.get("task") ?? "";
  const client = useExperimentClient();
  const url = useExperimentUrl();
  const navigate = useNavigate();
  const { canWrite } = useSpacePermissions(client.customerId);
  const agent = useAgentDeployed("agent-experiments");
  const task = useAgentTask(taskId, client.customerId, client.workspaceId);
  const [notice, setNotice] = useState(params.get("error") ?? "");
  const [busy, setBusy] = useState<string | null>(null);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [open, setOpen] = useState<"agent" | "experiment" | null>(null);
  const [search, setSearch] = useState("");
  const [confirmDialog, confirm] = useConfirm();
  // What an experiment can search: Orama's methodologies and the workspace's own (lake#689).
  const [methodology, setMethodology] = useState("M-1");
  const listedMethodologies = useWq<Row>("list_methodologies", {}, { retry: false, enabled: open === "experiment" });
  const studied = studyMethodologies(listedMethodologies.data?.methodologies);
  const chosen = studied.find((entry) => entry.id === methodology);
  const own = isWorkspaceMethodology(chosen) || /^W-[0-9a-f]{8}$/.test(methodology);
  const ownMetrics = own ? objectiveMetrics(chosen) : [];
  const listing = useQuery({
    queryKey: ["list_experiments", client.workspaceId, project, state],
    queryFn: () => client.read<{ experiments?: Experiment[]; runs?: Row[] }>("list_experiments", { project, state }),
    enabled: !!client.customerId,
  });
  const actions = useExperimentActions(() => listing.refetch(), setNotice);

  const run = async (key: string, work: () => Promise<void>) => {
    setBusy(key);
    setNotice("");
    try {
      await work();
    } catch (error) {
      setNotice(describe(error));
    } finally {
      setBusy(null);
    }
  };

  /** An Experiments Agent turn, then this page showing its task. */
  const turn = async (message: string, extra: Row = {}) => {
    const result = await client.act<Row>("agent_turn", { agent: "agent-experiments", kind: "experiment", message, ...extra });
    navigate(url("/train/experiments", { task: String(result.task_id ?? "") }));
  };

  const filter = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = new FormData(event.currentTarget);
    navigate(url("/train/experiments", { project: String(form.get("project") ?? ""), state }));
  };

  const startExperiment = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = event.currentTarget;
    void run("start", async () => {
      // As the old start script did: dotted names nest, numbers are numbers, empty values are left out.
      const input: Row = {};
      for (const element of Array.from(form.elements) as HTMLInputElement[]) {
        if (!element.name || element.name === "search_space" || element.name === "released_metrics") continue;
        const value = element.type === "checkbox" ? element.checked : element.type === "number" ? Number(element.value) : element.value.trim();
        if (value === "") continue;
        const [parent, child] = element.name.split(".");
        if (child) {
          input[parent] ||= {};
          input[parent][child] = value;
        } else {
          input[parent] = value;
        }
      }
      const search = (form.elements.namedItem("search_space") as HTMLTextAreaElement).value.trim();
      if (search) {
        try {
          input.search_space = JSON.parse(search);
        } catch (e) {
          throw new Error(`Search parameters: ${(e as Error).message}`);
        }
      } else {
        input.from_defaults = true;
      }
      input.released_metrics = (form.elements.namedItem("released_metrics") as HTMLInputElement).value
        .split(",")
        .map((value) => value.trim())
        .filter(Boolean);
      if (!(await confirm("Start training with these parameters and budgets?", "Confirm", JSON.stringify(input, null, 2)))) return;
      const experiment = await client.act<Row>("start_experiment", { ...input, confirm: true });
      navigate(url("/train/experiment", { experiment: String(experiment.id ?? "") }));
    });
  };

  const experiments = listing.data?.experiments ?? [];
  const runs = listing.data?.runs ?? [];
  const needle = search.trim().toLowerCase();
  const shownExperiments = experiments.filter((experiment) => !needle || `${cell(experiment.spec?.name)} ${cell(experiment.spec?.project)}`.toLowerCase().includes(needle));
  const shownRuns = runs.filter((item) => !needle || `${cell(item.id)} ${cell(item.project)}`.toLowerCase().includes(needle));
  const trialCount = experiments.reduce((sum, experiment) => sum + (experiment.trials ?? []).length, 0);
  const running = experiments.filter((experiment) => experiment.status === "running").length;

  const compare = () => {
    const chosen = [...selected];
    if (chosen.length < 2 || chosen.length > 8) {
      setNotice("Select two to eight runs to compare.");
      return;
    }
    navigate(url("/train/experiments/compare", { runs: chosen.join(",") }));
  };

  const experimentColumns: Column<Experiment>[] = [
    {
      key: "name",
      label: "Experiment",
      sort: (experiment) => cell(experiment.spec?.name),
      render: (experiment) => {
        const { objective } = experimentFigures(experiment);
        return (
          <div className="min-w-0">
            <Link to={url("/train/experiment", { experiment: experiment.id })} className="font-semibold">
              {cell(experiment.spec?.name)}
            </Link>
            <div className="truncate text-[12px] text-ink-3">
              {cell(experiment.spec?.project)} · {metricLabel(objective.metric)}
            </div>
          </div>
        );
      },
    },
    { key: "state", label: "State", sort: (experiment) => experiment.status, render: (experiment) => <Status value={experiment.status} /> },
    {
      key: "best",
      label: "Best",
      numeric: true,
      sort: (experiment) => experimentFigures(experiment).best,
      render: (experiment) => {
        const { best, objective } = experimentFigures(experiment);
        return (
          <div>
            <div className="font-mono font-semibold text-ink">{best === null ? "—" : cell(best)}</div>
            <div className="text-[11px] text-ink-3">{cell(objective.direction)}</div>
          </div>
        );
      },
    },
    {
      key: "trials",
      label: "Trials / Budget",
      numeric: true,
      sort: (experiment) => (experiment.trials ?? []).length,
      render: (experiment) => {
        const { counts, budgets } = experimentFigures(experiment);
        return (
          <div>
            <div className="font-mono">
              {(experiment.trials ?? []).length} / {cell(budgets.max_trials)}
            </div>
            <div className="text-[11px] text-ink-3" title="Complete / Pruned / Failed">
              {counts.complete ?? 0} complete · {counts.pruned ?? 0} pruned · {counts.failed ?? 0} failed
            </div>
          </div>
        );
      },
    },
    {
      key: "cost",
      label: "Cost / Cap",
      numeric: true,
      render: (experiment) => (
        <span className="font-mono">
          {cell(experiment.spent_cost)} / {cell(experimentFigures(experiment).budgets.cost_cap)}
        </span>
      ),
    },
    {
      key: "actions",
      label: "",
      render: (experiment) =>
        canWrite && experiment.status === "running" ? (
          <Button variant="danger" disabled={actions.busy !== null} onClick={() => void actions.perform("stop_experiment", experiment.id)}>
            Stop
          </Button>
        ) : null,
    },
  ];

  const runColumns: Column<Row>[] = [
    {
      key: "compare",
      label: "Compare",
      render: (item) => {
        const id = String(item.id ?? "");
        return (
          <input
            type="checkbox"
            aria-label={`Compare run ${id}`}
            checked={selected.has(id)}
            onChange={(e) =>
              setSelected((current) => {
                const next = new Set(current);
                if (e.target.checked) next.add(id);
                else next.delete(id);
                return next;
              })
            }
          />
        );
      },
    },
    {
      key: "run",
      label: "Run",
      sort: (item) => String(item.id ?? ""),
      render: (item) => (
        <div className="min-w-0">
          <Link to={url("/train/experiment/run", { run: String(item.id ?? "") })} className="font-mono">
            {String(item.id ?? "")}
          </Link>
          <div className="truncate text-[12px] text-ink-3">{cell(item.project)}</div>
        </div>
      ),
    },
    {
      key: "state",
      label: "State",
      sort: (item) => String(item.status ?? ""),
      render: (item) => (
        <span className="inline-flex flex-wrap items-center gap-1.5">
          <Status value={String(item.status ?? "")} />
          {item.metrics_suppressed !== undefined ? <span className="text-[12px] text-ink-3">curves suppressed for differential privacy</span> : null}
        </span>
      ),
    },
    { key: "created", label: "Created", sort: (item) => String(item.created ?? ""), render: (item) => <span title={cell(item.created)}>{relative(item.created, Date.now(), cell(item.created))}</span> },
  ];

  return (
    <ExperimentsShell
      mode="list"
      notice={open ? "" : notice}
      actions={
        <>
          <Button onClick={() => setOpen("agent")}>
            <Bot size={14} aria-hidden /> Ask the Experiments Agent
          </Button>
          {canWrite && (
            <Button variant="primary" onClick={() => setOpen("experiment")}>
              <Play size={14} aria-hidden /> Start experiment
            </Button>
          )}
        </>
      }
    >
      {confirmDialog}
      {actions.dialog}
      {taskId && <TaskPanels taskId={taskId} q={task} busy={busy} run={run} turn={turn} />}

      {listing.isLoading ? (
        <Skeleton />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="experiments" />
      ) : (
        <>
          <StatRow>
            <Stat label="Experiments" value={experiments.length} icon={FlaskConical} />
            <Stat label="Running" value={running} tone={running ? "ok" : undefined} />
            <Stat label="Trials" value={trialCount} sub="across these experiments" />
            <Stat label="Tracked runs" value={runs.length} sub="outside experiments" />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search experiments and runs…" label="Search experiments and runs" />
            <FilterChips
              options={STATES.filter(Boolean).map((value) => ({ id: value, label: value }))}
              value={state || null}
              onChange={(value) => navigate(url("/train/experiments", { project, state: value ?? "" }))}
              allLabel="All states"
            />
            <form className="ml-auto flex items-center gap-1.5" onSubmit={filter}>
              <input name="project" defaultValue={project} key={`project-${project}`} placeholder="Project" aria-label="Project" className="search-input" />
              <Button type="submit">Filter</Button>
            </form>
          </Toolbar>

          <div className="space-y-4">
            <Card title="Experiments" padded={false}>
              <DataTable
                rows={shownExperiments}
                q={listing}
                what="experiments"
                columns={experimentColumns}
                rowKey={(experiment) => experiment.id}
                caption="Experiments"
                empty={
                  <div className="p-4">
                    <Empty
                      compact
                      icon={FlaskConical}
                      title="No Experiments"
                      action={
                        canWrite ? (
                          <Button variant="primary" onClick={() => setOpen("experiment")}>
                            Start experiment
                          </Button>
                        ) : undefined
                      }
                    >
                      {experiments.length ? "Clear the search to see more." : "Start an experiment, or ask the Experiments Agent to propose one."}
                    </Empty>
                  </div>
                }
              />
            </Card>
            <Card
              title="Tracked Runs Outside Experiments"
              padded={false}
              actions={
                <Button onClick={compare} disabled={selected.size === 0}>
                  <GitCompare size={14} aria-hidden /> Compare selected runs{selected.size ? ` (${selected.size})` : ""}
                </Button>
              }
              footer="Select two to eight runs. Trial runs can also be compared from their experiment."
            >
              <DataTable
                rows={shownRuns}
                q={listing}
                what="runs"
                columns={runColumns}
                rowKey={(item) => String(item.id ?? "")}
                caption="Tracked runs"
                empty={<p className="muted m-0 px-4 py-3 text-[13px]">No tracked runs outside experiments.</p>}
              />
            </Card>
          </div>
        </>
      )}

      {open === "agent" && (
        <Drawer
          title="Run an Experiment"
          subtitle="Describe the training or tuning goal. The Experiments Agent proposes a complete experiment for you to edit and confirm before it starts."
          onClose={() => setOpen(null)}
          footer={
            <>
              <Button onClick={() => setOpen(null)}>Cancel</Button>
              <Button variant="primary" type="submit" form="experiment-goal" disabled={!agent.deployed} loading={busy === "goal"}>
                Ask the Experiments Agent
              </Button>
            </>
          }
        >
          {notice && <Banner tone="info">{notice}</Banner>}
          <form
            id="experiment-goal"
            className="stack"
            onSubmit={(event) => {
              event.preventDefault();
              const goal = String(new FormData(event.currentTarget).get("goal") ?? "");
              void run("goal", async () => {
                await turn(goal);
                setOpen(null);
              });
            }}
          >
            <label>
              Goal
              <textarea name="goal" rows={5} placeholder="Tune a churn model for validation AUC with a bounded trial budget." required disabled={!agent.deployed} />
            </label>
          </form>
          {!agent.deployed && !agent.loading && <Notice tone="warning">The Experiments Agent is not running on this Lake.</Notice>}
        </Drawer>
      )}

      {open === "experiment" && canWrite && (
        <Drawer
          title="Start an Experiment"
          subtitle="Choose the pipeline, data, objective and budgets. You confirm the parameters before training starts."
          width={640}
          onClose={() => setOpen(null)}
          footer={
            <>
              <Button onClick={() => setOpen(null)}>Cancel</Button>
              <Button variant="primary" type="submit" form="start-experiment" loading={busy === "start"}>
                Review and start experiment
              </Button>
            </>
          }
        >
          {notice && <Banner tone="info">{notice}</Banner>}
          <Banner tone="quiet">
            Selection uses the validation split. The final test split is never used for tuning. Metrics are named as the run report names them: held_out_auc, held_out_log_loss,
            held_out_accuracy, held_out_rmse, held_out_mae.
          </Banner>
          {own && (
            <Banner tone="quiet">
              {cell(chosen?.name ?? methodology)} is your workspace's own methodology. Each trial runs its Python code as a training run in its sandbox, metered like any other run, and reports
              its held-out metrics back. Its trials are compared on the rows each run holds out.
            </Banner>
          )}
          <form id="start-experiment" className="experiments" onSubmit={startExperiment}>
            <div className="experiment-fields" key={methodology}>
              <label>
                Experiment name
                <input name="name" required />
              </label>
              <label>
                Methodology
                {studied.length > 0 ? (
                  <select name="methodology" value={methodology} onChange={(e) => setMethodology(e.target.value)} required>
                    {studied.map((entry) => (
                      <option key={String(entry.id)} value={String(entry.id)}>
                        {String(entry.id)} · {cell(entry.name)}
                        {isWorkspaceMethodology(entry) ? " (yours)" : ""}
                      </option>
                    ))}
                  </select>
                ) : (
                  <input name="methodology" value={methodology} onChange={(e) => setMethodology(e.target.value.trim())} required />
                )}
              </label>
              <label>
                Project{own ? " (optional)" : ""}
                <input name="project" required={!own} />
              </label>
              <label>
                Pipeline{own ? " (optional)" : ""}
                <input name="pipeline" required={!own} />
              </label>
              <label>
                Dataset
                <input name="dataset" placeholder="database.table" required />
              </label>
              {own && (
                <label>
                  Column to predict{needsTargetColumn(chosen) ? "" : " (optional)"}
                  <input name="target" required={needsTargetColumn(chosen)} />
                </label>
              )}
              <label>
                Objective metric
                {own && ownMetrics.length > 0 ? (
                  <select name="objective.metric" defaultValue={ownMetrics[0]} required>
                    {ownMetrics.map((metric) => (
                      <option key={metric} value={metric}>
                        {metric}
                      </option>
                    ))}
                  </select>
                ) : (
                  <input name="objective.metric" defaultValue="held_out_auc" list="metric-names" required />
                )}
              </label>
              <datalist id="metric-names">
                {METRIC_NAMES.map(([name]) => (
                  <option key={name} value={name} />
                ))}
              </datalist>
              <label>
                Direction
                <select name="objective.direction" defaultValue={own && ownMetrics.length > 0 && minimized(ownMetrics[0]) ? "minimize" : "maximize"}>
                  <option value="maximize">Maximize</option>
                  <option value="minimize">Minimize</option>
                </select>
              </label>
              <label>
                Validation split
                <input name="objective.validation_split" defaultValue="validation" readOnly={own} required />
              </label>
              <label>
                Sampler
                <select name="sampler">
                  <option>tpe</option>
                  <option>random</option>
                  <option>grid</option>
                </select>
              </label>
              <label>
                Pruner
                {own ? (
                  <select name="pruner" defaultValue="none" disabled title="A runner trial reports one result, so nothing is pruned part-way">
                    <option>none</option>
                  </select>
                ) : (
                  <select name="pruner">
                    <option>asha</option>
                    <option>median</option>
                    <option>none</option>
                  </select>
                )}
              </label>
              <label>
                Trial budget
                <input name="budgets.max_trials" type="number" min={1} defaultValue="10" required />
              </label>
              <label>
                Parallel trials
                <input name="budgets.max_parallel_trials" type="number" min={1} defaultValue="1" required />
              </label>
              <label>
                Wall-time budget (ms; 0 is unlimited)
                <input name="budgets.max_wall_time_ms" type="number" min={0} defaultValue="0" required />
              </label>
              <label>
                Cost cap (0 is unlimited)
                <input name="budgets.cost_cap" type="number" min={0} step="any" defaultValue="0" required />
              </label>
              <label>
                Seed
                <input name="seed" type="number" min={0} defaultValue="1138" required />
              </label>
            </div>
            {own && searchedParameters(chosen).length > 0 && (
              <div className="stack">
                <span className="text-[12.5px] text-ink-2">Parameters it searches, as its manifest declares them</span>
                <ul className="m-0 pl-4 text-[13px]">
                  {searchedParameters(chosen).map((parameter) => (
                    <li key={parameter.name}>
                      <span className="font-mono">{parameter.name}</span> · {parameter.kind} · {parameterRange(parameter)}
                    </li>
                  ))}
                </ul>
              </div>
            )}
            <label>
              Search parameters (JSON; leave blank for {own ? "the declared ranges" : "methodology defaults"})
              <textarea name="search_space" rows={4} />
            </label>
            {!own && (
              <label>
                <input type="checkbox" name="differential_privacy" /> Differential privacy
              </label>
            )}
            <label hidden={own}>
              Released metrics for private experiments (comma separated)
              <input name="released_metrics" />
            </label>
          </form>
        </Drawer>
      )}
    </ExperimentsShell>
  );
}

/** The Experiments Agent task named in the URL: working, its proposal to edit and confirm, or its
 * result with a reply. */
function TaskPanels({
  taskId,
  q,
  busy,
  run,
  turn,
}: {
  taskId: string;
  q: ReturnType<typeof useAgentTask>;
  busy: string | null;
  run: (key: string, work: () => Promise<void>) => Promise<void>;
  turn: (message: string, extra?: Row) => Promise<void>;
}) {
  const client = useExperimentClient();
  const url = useExperimentUrl();
  if (q.isLoading) return <Skeleton lines={2} />;
  if (q.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what="the Experiments Agent task" />;
  const task = q.data as AgentTask | undefined;
  if (!task) return null;
  if (task.agent !== "agent-experiments" || task.kind !== "experiment") {
    return (
      <p className="error" role="alert">
        That is not an Experiments Agent experiment task.
      </p>
    );
  }
  const link = <AgentEventsLink customerId={client.customerId} taskId={taskId} />;
  const reply = taskReply(task);
  const result = taskResult(task);
  if (taskWorking(task)) {
    return (
      <div className="mb-5">
        <Card tone="active" label="Experiments Agent" title="Experiments Agent Is Working" actions={<Loader2 size={16} className="animate-spin text-action" aria-hidden />} footer={link}>
          <div aria-live="polite">
            <p className="muted m-0">It keeps working if you leave; this page checks again every 2 seconds.</p>
            <p role="status" className="mb-0">
              {task.message ?? ""}
            </p>
          </div>
        </Card>
      </div>
    );
  }
  // Experiments Agent's answer carries the experiment it proposes as the engine's `study`.
  const proposed = result.study && typeof result.study === "object" && !Array.isArray(result.study);
  if (result.kind === "proposal" && result.confirm === true && proposed) {
    const shown = result;
    const confirmExperiment = (event: FormEvent<HTMLFormElement>) => {
      event.preventDefault();
      const edited = String(new FormData(event.currentTarget).get("experiment") ?? "");
      void run("confirm", async () => {
        // The confirm form's checks, against a fresh read of the task.
        const fresh = await fetchAgentTask(taskId, client.customerId, client.workspaceId);
        const proposal = taskResult(fresh);
        const freshProposed = proposal.study && typeof proposal.study === "object" && !Array.isArray(proposal.study);
        if (fresh.agent !== "agent-experiments" || fresh.kind !== "experiment" || fresh.status !== "succeeded" || proposal.kind !== "proposal" || proposal.confirm !== true || !freshProposed) {
          throw new ApiError(400, "invalid_input", "That task has no Experiments Agent proposal to confirm.");
        }
        if (canonicalJson(proposal) !== canonicalJson(shown)) throw new ApiError(409, "conflict", "The Experiments Agent proposal changed before confirmation.");
        let experiment: unknown;
        try {
          experiment = JSON.parse(edited);
        } catch {
          experiment = null;
        }
        if (!experiment || typeof experiment !== "object" || Array.isArray(experiment)) throw new ApiError(400, "invalid_input", "The edited experiment proposal must be a JSON object.");
        // lake checks the task and digest against the stored proposal before the experiment can start.
        await turn("Confirm and start the reviewed experiment proposal.", {
          proposal_task_id: taskId,
          proposal_sha256: task.proposal_sha256 ?? "",
          context: { confirmed_proposal: { ...proposal, study: experiment } },
        });
      });
    };
    return (
      <div className="mb-5" id="experiment-proposal">
        <Card tone="active" label="Experiments Agent proposal" title="Proposed Experiment" footer={link}>
          <div className="ask-reply">
            <Markdown>{reply}</Markdown>
          </div>
          <p className="muted text-[13px]">Edit the experiment JSON if needed. The reviewed object is validated again before the agent can start it.</p>
          <form className="stack" onSubmit={confirmExperiment}>
            <label>
              Experiment proposal (JSON)
              <textarea name="experiment" rows={18} required defaultValue={JSON.stringify(result.study, null, 2)} className="font-mono" />
            </label>
            <details>
              <summary className="cursor-pointer text-[13px] text-ink-2">Why these choices</summary>
              <pre>{JSON.stringify(result.explanations ?? {}, null, 2)}</pre>
            </details>
            <div>
              <Button variant="primary" type="submit" loading={busy === "confirm"}>
                <Play size={14} aria-hidden /> Confirm and start experiment
              </Button>
            </div>
          </form>
        </Card>
      </div>
    );
  }
  const experiment = (proposed ? result.study : {}) as Row;
  return (
    <div className="mb-5">
      <Card
        label="Experiments Agent"
        title="Experiments Agent Result"
        actions={
          result.kind === "running" && experiment.id ? (
            <Button to={url("/train/experiment", { experiment: String(experiment.id) })}>Open the running experiment</Button>
          ) : undefined
        }
        footer={link}
      >
        <div className="ask-reply">
          <Markdown>{reply}</Markdown>
        </div>
        {needsReply(task, "agent-experiments") && (
          <form
            className="stack mt-3"
            onSubmit={(event) => {
              event.preventDefault();
              const message = String(new FormData(event.currentTarget).get("reply") ?? "");
              void run("reply", async () => {
                const fresh = await fetchAgentTask(taskId, client.customerId, client.workspaceId);
                if (fresh.agent !== "agent-experiments" || fresh.kind !== "experiment" || !needsReply(fresh, "agent-experiments") || !fresh.conversation_id) {
                  throw new ApiError(400, "invalid_input", "That task has no Experiments Agent question to answer.");
                }
                await turn(message, { conversation_id: fresh.conversation_id });
              });
            }}
          >
            <label>
              Reply
              <textarea name="reply" rows={4} required />
            </label>
            <div>
              <Button variant="primary" type="submit" loading={busy === "reply"}>
                Reply to the Experiments Agent
              </Button>
            </div>
          </form>
        )}
      </Card>
    </div>
  );
}
