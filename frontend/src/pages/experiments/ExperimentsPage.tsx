/** Train → Experiments (web_ui_experiments.cpp list mode; serve_experiments_agent_form; the
 * start-study and compare scripts): ExperimentsAgent's turn with its editable proposal and
 * confirmed start, the filter, Start a Study, the studies with Stop, and the tracked runs. */
import { type FormEvent, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { ApiError, type Row } from "@/lib/api";
import { useQuery } from "@tanstack/react-query";
import { ErrorBlock, Notice, Skeleton } from "@/lib/ui";
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

const STATES = ["", "pending", "running", "succeeded", "failed", "stopped", "complete"];

type Study = { id: string; spec: Row; status: string; trials: Row[]; spent_cost?: unknown };

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
  const [confirmDialog, confirm] = useConfirm();
  const listing = useQuery({
    queryKey: ["list_experiments", client.workspaceId, project, state],
    queryFn: () => client.read<{ studies?: Study[]; runs?: Row[] }>("list_experiments", { project, state }),
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

  /** An ExperimentsAgent turn, then this page showing its task. */
  const turn = async (message: string, extra: Row = {}) => {
    const result = await client.act<Row>("agent_turn", { agent: "agent-experiments", kind: "experiment", message, ...extra });
    navigate(url("/train/experiments", { task: String(result.task_id ?? "") }));
  };

  const filter = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = new FormData(event.currentTarget);
    navigate(url("/train/experiments", { project: String(form.get("project") ?? ""), state: String(form.get("state") ?? "") }));
  };

  const startStudy = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = event.currentTarget;
    void run("start", async () => {
      // The start-study script: dotted names nest, numbers are numbers, empty values are left out.
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
      const study = await client.act<Row>("start_study", { ...input, confirm: true });
      navigate(url("/train/experiment", { study: String(study.id ?? "") }));
    });
  };

  const studies = listing.data?.studies ?? [];
  const runs = listing.data?.runs ?? [];

  return (
    <ExperimentsShell mode="list" notice={notice}>
      {confirmDialog}
      {actions.dialog}
      <section className="panel">
        <h2>Run an Experiment</h2>
        <p className="sub">Describe the training or tuning goal. ExperimentsAgent proposes a complete study for you to edit and confirm before it starts.</p>
        <form
          className="stack"
          onSubmit={(event) => {
            event.preventDefault();
            const goal = String(new FormData(event.currentTarget).get("goal") ?? "");
            void run("goal", () => turn(goal));
          }}
        >
          <label>
            Goal
            <textarea name="goal" rows={4} placeholder="Tune a churn model for validation AUC with a bounded trial budget." required disabled={!agent.deployed} />
          </label>
          <button type="submit" disabled={!agent.deployed || busy === "goal"}>
            Ask ExperimentsAgent
          </button>
        </form>
        {!agent.deployed && !agent.loading && <Notice tone="warning">ExperimentsAgent is not registered on this lake yet.</Notice>}
      </section>
      {taskId && <TaskPanels taskId={taskId} q={task} busy={busy} run={run} turn={turn} />}

      <form className="experiment-fields" onSubmit={filter}>
        <label>
          Project
          <input name="project" defaultValue={project} key={`project-${project}`} />
        </label>
        <label>
          State
          <select name="state" defaultValue={state} key={`state-${state}`}>
            {STATES.map((value) => (
              <option key={value} value={value}>
                {value || "All states"}
              </option>
            ))}
          </select>
        </label>
        <button type="submit">Filter</button>
      </form>

      {canWrite && (
        <details className="panel">
          <summary>Start a Study</summary>
          <form onSubmit={startStudy}>
            <div className="experiment-fields">
              <label>
                Study name
                <input name="name" required />
              </label>
              <label>
                Project
                <input name="project" required />
              </label>
              <label>
                Pipeline
                <input name="pipeline" required />
              </label>
              <label>
                Dataset
                <input name="dataset" required />
              </label>
              <label>
                Methodology
                <input name="methodology" defaultValue="M-1" required />
              </label>
              <label>
                Objective metric
                <input name="objective.metric" defaultValue="held_out_auc" list="metric-names" required />
              </label>
              <datalist id="metric-names">
                {METRIC_NAMES.map(([name]) => (
                  <option key={name} value={name} />
                ))}
              </datalist>
              <label>
                Direction
                <select name="objective.direction">
                  <option value="maximize">Maximize</option>
                  <option value="minimize">Minimize</option>
                </select>
              </label>
              <label>
                Validation split
                <input name="objective.validation_split" defaultValue="validation" required />
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
                <select name="pruner">
                  <option>asha</option>
                  <option>median</option>
                  <option>none</option>
                </select>
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
            <label>
              Search parameters (JSON; leave blank for methodology defaults)
              <textarea name="search_space" rows={4} />
            </label>
            <label>
              <input type="checkbox" name="differential_privacy" /> Differential privacy
            </label>
            <label>
              Released metrics for private studies (comma separated)
              <input name="released_metrics" />
            </label>
            <p>
              Selection uses the validation split. The final test split is never used for tuning. Metrics are named as the run report names them: held_out_auc, held_out_log_loss,
              held_out_accuracy, held_out_rmse, held_out_mae.
            </p>
            <button type="submit" disabled={busy === "start"}>
              Review and start study
            </button>
          </form>
        </details>
      )}

      {listing.isLoading ? (
        <Skeleton />
      ) : listing.isError ? (
        <ErrorBlock error={listing.error} retry={() => void listing.refetch()} what="experiments" />
      ) : (
        <>
          <section className="panel">
            <h2>Studies</h2>
            <div className="table-scroll">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Project</th>
                    <th>State</th>
                    <th>Objective</th>
                    <th>Best</th>
                    <th>Complete / Pruned / Failed</th>
                    <th>Trials / Budget</th>
                    <th>Cost / Cap</th>
                    <th>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {studies.map((study) => {
                    const spec = study.spec ?? {};
                    const objective = (spec.objective ?? {}) as Row;
                    const budgets = (spec.budgets ?? {}) as Row;
                    const counts: Record<string, number> = {};
                    let best: number | null = null;
                    for (const trial of study.trials ?? []) {
                      counts[trial.state] = (counts[trial.state] ?? 0) + 1;
                      if (trial.state === "complete" && typeof trial.objective === "number") {
                        if (best === null || (objective.direction === "maximize" ? trial.objective > best : trial.objective < best)) best = trial.objective;
                      }
                    }
                    return (
                      <tr key={study.id}>
                        <td>
                          <Link to={url("/train/experiment", { study: study.id })}>{cell(spec.name)}</Link>
                        </td>
                        <td>{cell(spec.project)}</td>
                        <td>{cell(study.status)}</td>
                        <td>{metricLabel(objective.metric)}</td>
                        <td>{best === null ? "—" : cell(best)}</td>
                        <td>
                          {counts.complete ?? 0} / {counts.pruned ?? 0} / {counts.failed ?? 0}
                        </td>
                        <td>
                          {(study.trials ?? []).length} / {cell(budgets.max_trials)}
                        </td>
                        <td>
                          {cell(study.spent_cost)} / {cell(budgets.cost_cap)}
                        </td>
                        <td>
                          {canWrite && study.status === "running" && (
                            <button type="button" disabled={actions.busy !== null} onClick={() => void actions.perform("stop_study", study.id)}>
                              Stop
                            </button>
                          )}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </section>
          <section className="panel">
            <h2>Tracked Runs Outside Studies</h2>
            <form
              onSubmit={(event) => {
                event.preventDefault();
                const chosen = [...selected];
                if (chosen.length < 2 || chosen.length > 8) {
                  setNotice("Select two to eight runs to compare.");
                  return;
                }
                navigate(url("/train/experiments/compare", { runs: chosen.join(",") }));
              }}
            >
              <div className="table-scroll">
                <table>
                  <thead>
                    <tr>
                      <th>Compare</th>
                      <th>Run</th>
                      <th>Project</th>
                      <th>State</th>
                      <th>Created</th>
                    </tr>
                  </thead>
                  <tbody>
                    {runs.map((item) => {
                      const id = String(item.id ?? "");
                      return (
                        <tr key={id}>
                          <td>
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
                          </td>
                          <td>
                            <Link to={url("/train/experiment/run", { run: id })}>{id}</Link>
                          </td>
                          <td>{cell(item.project)}</td>
                          <td>
                            {cell(item.status)}
                            {item.metrics_suppressed !== undefined ? " · curves suppressed for differential privacy" : ""}
                          </td>
                          <td>{cell(item.created)}</td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
              <button type="submit">Compare selected runs</button>
              <p>Select two to eight runs. Trial runs can also be compared from their study.</p>
            </form>
          </section>
        </>
      )}
    </ExperimentsShell>
  );
}

/** The ExperimentsAgent task named in the URL: working, its proposal to edit and confirm, or its
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
  if (q.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what="the ExperimentsAgent task" />;
  const task = q.data as AgentTask | undefined;
  if (!task) return null;
  if (task.agent !== "agent-experiments" || task.kind !== "experiment") {
    return (
      <p className="error" role="alert">
        That is not an ExperimentsAgent experiment task.
      </p>
    );
  }
  const link = <AgentEventsLink customerId={client.customerId} taskId={taskId} />;
  const reply = taskReply(task);
  const result = taskResult(task);
  if (taskWorking(task)) {
    return (
      <>
        {link}
        <section className="panel" aria-live="polite">
          <h2>ExperimentsAgent Is Working</h2>
          <p>It keeps working if you leave; this page checks again every 2 seconds.</p>
          <p role="status">{task.message ?? ""}</p>
        </section>
      </>
    );
  }
  const studyObject = result.study && typeof result.study === "object" && !Array.isArray(result.study);
  if (result.kind === "proposal" && result.confirm === true && studyObject) {
    const shown = result;
    const confirmStudy = (event: FormEvent<HTMLFormElement>) => {
      event.preventDefault();
      const edited = String(new FormData(event.currentTarget).get("study") ?? "");
      void run("confirm", async () => {
        // The confirm form's checks, against a fresh read of the task.
        const fresh = await fetchAgentTask(taskId, client.customerId, client.workspaceId);
        const proposal = taskResult(fresh);
        const freshStudy = proposal.study && typeof proposal.study === "object" && !Array.isArray(proposal.study);
        if (fresh.agent !== "agent-experiments" || fresh.kind !== "experiment" || fresh.status !== "succeeded" || proposal.kind !== "proposal" || proposal.confirm !== true || !freshStudy) {
          throw new ApiError(400, "invalid_input", "That task has no ExperimentsAgent proposal to confirm.");
        }
        if (canonicalJson(proposal) !== canonicalJson(shown)) throw new ApiError(409, "conflict", "The ExperimentsAgent proposal changed before confirmation.");
        let study: unknown;
        try {
          study = JSON.parse(edited);
        } catch {
          study = null;
        }
        if (!study || typeof study !== "object" || Array.isArray(study)) throw new ApiError(400, "invalid_input", "The edited study proposal must be a JSON object.");
        // lake checks the task and digest against the stored proposal before the study can start.
        await turn("Confirm and start the reviewed study proposal.", {
          proposal_task_id: taskId,
          proposal_sha256: task.proposal_sha256 ?? "",
          context: { confirmed_proposal: { ...proposal, study } },
        });
      });
    };
    return (
      <>
        {link}
        <section className="panel" id="experiment-proposal">
          <h2>Proposed Study</h2>
          <div className="ask-reply">
            <Markdown>{reply}</Markdown>
          </div>
          <p>Edit the study JSON if needed. The reviewed object is validated again before the agent can start it.</p>
          <form className="stack" onSubmit={confirmStudy}>
            <label>
              Study proposal (JSON)
              <textarea name="study" rows={18} required defaultValue={JSON.stringify(result.study, null, 2)} />
            </label>
            <details>
              <summary>Why these choices</summary>
              <pre>{JSON.stringify(result.explanations ?? {}, null, 2)}</pre>
            </details>
            <button className="button button-primary" type="submit" disabled={busy === "confirm"}>
              Confirm and start study
            </button>
          </form>
        </section>
      </>
    );
  }
  const study = (studyObject ? result.study : {}) as Row;
  return (
    <>
      {link}
      <section className="panel">
        <h2>ExperimentsAgent Result</h2>
        <div className="ask-reply">
          <Markdown>{reply}</Markdown>
        </div>
        {result.kind === "running" && study.id && (
          <p>
            <Link to={url("/train/experiment", { study: String(study.id) })}>Open the running study</Link>
          </p>
        )}
        {needsReply(task, "agent-experiments") && (
          <>
            <h3>Reply</h3>
            <form
              className="stack"
              onSubmit={(event) => {
                event.preventDefault();
                const message = String(new FormData(event.currentTarget).get("reply") ?? "");
                void run("reply", async () => {
                  const fresh = await fetchAgentTask(taskId, client.customerId, client.workspaceId);
                  if (fresh.agent !== "agent-experiments" || fresh.kind !== "experiment" || !needsReply(fresh, "agent-experiments") || !fresh.conversation_id) {
                    throw new ApiError(400, "invalid_input", "That task has no ExperimentsAgent question to answer.");
                  }
                  await turn(message, { conversation_id: fresh.conversation_id });
                });
              }}
            >
              <label>
                Reply
                <textarea name="reply" rows={4} required />
              </label>
              <button className="button button-primary" type="submit" disabled={busy === "reply"}>
                Reply to ExperimentsAgent
              </button>
            </form>
          </>
        )}
      </section>
    </>
  );
}
