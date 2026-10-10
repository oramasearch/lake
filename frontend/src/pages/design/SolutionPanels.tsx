/** A solution project's panels on the project page (lake's active_solution_content and
 * solution_proposal_content): its stages and Run, the decision policy, the experiment and holdout,
 * the actions and A1 queue, the monitor and the ROI readout; or, before it exists, the Project
 * Agent's foundation proposal to confirm. Every confirmation the form handlers enforced is enforced
 * here before the call. */
import { useState } from "react";
import { act } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, ErrorBlock, Field, Form, Skeleton } from "@/lib/ui";
import { useScope, useWq, wa, useScopedRead } from "./api";
import {
  a1Queue,
  actionCounts,
  allocationShare,
  cellText,
  certificateGroups,
  countText,
  decisionsRef,
  experimentAllocations,
  latestActions,
  ledgerEntries,
  missingPredicates,
  monitorHealthy,
  parseNumber,
  policyConfig,
  proposedFoundation,
  text,
  type J,
  type SolutionProject,
} from "./logic";
import { CheckTable, FormError, JsonDisclosure, Panel, Refusal, RunBadge, useSubmit } from "./parts";

/* ------------------------------------------------------------------ the foundation proposal */

/** The `solution:<project>` task: working, failed, or its proposal with the confirm form. */
export function ProposalContent({ project, task }: { project: string; task: J }) {
  const status = text(task, "status");
  if (status === "pending" || status === "running") {
    return (
      <section className="panel notice" role="status">
        <h2>Foundation Proposal</h2>
        <p>
          <strong>The Project Agent is reading the dataset…</strong> It is proposing the snapshot boundary, column roles, methodology inputs and policy predicates. Nothing is
          created until you confirm. This page checks again every 3 seconds.
        </p>
        <p>{text(task, "message")}</p>
      </section>
    );
  }
  if (status === "failed") {
    return (
      <p className="error" role="alert">
        The Project Agent could not propose the foundation: {text(task, "message")}
      </p>
    );
  }
  const proposal = proposedFoundation(task);
  if (!Object.keys(proposal).length) {
    return status === "succeeded" ? (
      <p className="error" role="alert">
        The Project Agent returned no foundation proposal.
      </p>
    ) : null;
  }
  return <FoundationForm project={project} task={task} proposal={proposal} />;
}

function FoundationForm({ project, task, proposal }: { project: string; task: J; proposal: J }) {
  const scope = useScope();
  const toast = useToast();
  const [foundation, setFoundation] = useState(() => JSON.stringify(proposal, null, 2));
  const [confirmed, setConfirmed] = useState(false);
  const { busy, error, run } = useSubmit();
  const missing = missingPredicates(task, proposal).join(", ");
  const submit = () =>
    run(async () => {
      let edits: unknown;
      try {
        edits = JSON.parse(foundation);
      } catch {
        edits = null;
      }
      if (!edits || typeof edits !== "object" || Array.isArray(edits)) throw new Refusal("The foundation must be a JSON object.");
      if (!confirmed) throw new Refusal("Confirm that you reviewed the foundation.");
      await act("confirm_solution", { task_id: text(task, "task_id"), workspace_id: scope.workspaceId, edits });
      toast(`Solution confirmed for ${project}`);
    });
  return (
    <Panel title="Foundation Proposal">
      <p>Review and edit the exact foundation sent to the engine. Delivery and economics are your inputs from Start.</p>
      {missing && (
        <p className="error" role="alert">
          <strong>Actions will remain blocked.</strong> The dataset is missing these exclusion predicates: {missing}. They are not treated as false.
        </p>
      )}
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="Foundation (JSON)" required>
          <textarea name="foundation" rows={28} value={foundation} onChange={(e) => setFoundation(e.target.value)} />
        </Field>
        <label>
          <input type="checkbox" name="confirm" value="yes" required checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} /> I reviewed this snapshot, these
          roles, predicates, delivery settings and economics.
        </label>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Confirm and create solution
          </Button>
        </div>
      </Form>
    </Panel>
  );
}

/* ------------------------------------------------------------------ the active solution */

/** The solution's governed state, read from the ids its manifest carries. `live` re-reads it every
 * 3 seconds while a run is active, as the server page's refresh did. */
export function ActiveSolution({ project, solution, task, runs, live }: { project: string; solution: SolutionProject; task: J | null; runs: J[]; live: boolean }) {
  const f = solution.foundation;
  const interval = live ? 3000 : false;
  const policy = useWq<J>("get_policy", { policy_id: text(f, "policy_id") }, { refetchInterval: interval });
  const experiment = useWq<J>("get_experiment", { experiment: text(f, "experiment_id") }, { refetchInterval: interval });
  const ledger = useWq<J>("list_ledger", {}, { refetchInterval: interval });
  const monitor = useWq<J>("get_monitor", { solution: text(f, "monitor_id") }, { refetchInterval: interval });
  const roi = useWq<J>("get_roi_readout", { solution: text(f, "readout_id") || text(f, "scope") }, { refetchInterval: interval });
  // The latest run's decisions, from the table its "solution decisions" output names.
  const ref = decisionsRef(runs[0]);
  const decisions = useScopedRead<{ rows?: J[] }>(ref ? "workspace_sql" : null, { statement: `SELECT * FROM ${ref}` }, { refetchInterval: interval });

  const proposal = proposedFoundation(task);
  const missing: unknown[] = Array.isArray(solution.sources?.missing_policy_predicates) ? solution.sources.missing_policy_predicates : missingPredicates(task, proposal);

  const loading = policy.isLoading || experiment.isLoading || ledger.isLoading || monitor.isLoading || roi.isLoading;
  return (
    <>
      <Panel title="Solution Stages">
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Stage</th>
                <th>Name</th>
                <th>Kind</th>
              </tr>
            </thead>
            <tbody>
              {solution.stages.map((stage) => (
                <tr key={stage.id}>
                  <td>{cellText(stage.stage)}</td>
                  <td>{stage.name}</td>
                  <td>{stage.kind}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        {missing.length === 0 ? (
          <RunSolution project={project} graph={solution.graph} />
        ) : (
          <p className="error" role="alert">
            Run is blocked because an exclusion predicate is missing. Missing evidence is never treated as false.
          </p>
        )}
      </Panel>
      {loading && <Skeleton />}
      {policy.data && Object.keys(policy.data).length > 0 && <PolicyPanel project={project} policy={policy.data} />}
      {experiment.data && (
        <Panel title="Experiment and Holdout">
          <ExperimentSummary experiment={experiment.data} />
          <JsonDisclosure value={experiment.data} />
        </Panel>
      )}
      {!ledger.isLoading && <DecisionsPanel ledger={ledger.data} decisions={decisions.data?.rows?.slice(0, 100)} scope={text(f, "scope")} />}
      {monitor.data && (
        <Panel title="Monitor">
          <MonitorSummary monitor={monitor.data} />
          <JsonDisclosure value={monitor.data} />
        </Panel>
      )}
      {roi.data && (
        <Panel title="ROI Readout">
          <RoiSummary readout={roi.data} />
          <JsonDisclosure value={roi.data} />
        </Panel>
      )}
      {decisions.isError && <ErrorBlock error={decisions.error} what="the latest run's decisions" />}
    </>
  );
}

function RunSolution({ project, graph }: { project: string; graph: string }) {
  const scope = useScope();
  const toast = useToast();
  const [token, setToken] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      await wa(scope, "run_solution", { project, graph, ...(token ? { delivery_token: token } : {}) });
      setToken("");
      toast("Solution run started");
    });
  return (
    <Form onSubmit={submit}>
      <FormError error={error} />
      <Field label="Delivery token (optional, used for this run only)">
        <input type="password" name="delivery_token" autoComplete="off" value={token} onChange={(e) => setToken(e.target.value)} />
      </Field>
      <div>
        <Button type="submit" variant="primary" loading={busy}>
          Run
        </Button>
      </div>
    </Form>
  );
}

function PolicyPanel({ project, policy }: { project: string; policy: J }) {
  const config = policyConfig(policy);
  const tiers: J[] = Array.isArray(config?.tiers) ? config.tiers : [];
  return (
    <Panel title="Decision Policy">
      <dl>
        <dt>Version</dt>
        <dd>{cellText(policy.current_version ?? 0)}</dd>
        <dt>Autonomy</dt>
        <dd>{text(config, "autonomy") || "—"}</dd>
        <dt>Score threshold</dt>
        <dd>{config && "threshold" in config ? cellText(config.threshold) : "—"}</dd>
      </dl>
      {tiers.length > 0 && (
        <>
          <h3>Tiers and Thresholds</h3>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Tier</th>
                  <th>Minimum Score</th>
                  <th>Action</th>
                </tr>
              </thead>
              <tbody>
                {tiers.map((tier, index) => (
                  <tr key={index}>
                    <td>{text(tier, "name")}</td>
                    <td>{cellText(tier.minimum_score ?? 0)}</td>
                    <td>{text(tier, "action")}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
      <JsonDisclosure value={policy} />
      <details>
        <summary>Propose a New Policy Version</summary>
        <PolicyForm key={JSON.stringify(config)} project={project} policyId={text(policy, "id")} config={config} />
      </details>
    </Panel>
  );
}

function PolicyForm({ project, policyId, config }: { project: string; policyId: string; config: J }) {
  const scope = useScope();
  const toast = useToast();
  const [value, setValue] = useState(() => JSON.stringify(config ?? {}, null, 2));
  const [reason, setReason] = useState("");
  const [approved, setApproved] = useState(false);
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      let parsed: unknown;
      try {
        parsed = JSON.parse(value);
      } catch {
        parsed = null;
      }
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Refusal("The policy configuration must be a JSON object.");
      if (!approved) throw new Refusal("Approve the proposed policy version before applying it.");
      await wa(scope, "update_policy", { policy_id: policyId, config: parsed, reason });
      setReason("");
      setApproved(false);
      toast(`Policy version created for ${project}`);
    });
  return (
    <Form onSubmit={submit}>
      <FormError error={error} />
      <Field label="Configuration (JSON)" required>
        <textarea name="config" rows={16} value={value} onChange={(e) => setValue(e.target.value)} />
      </Field>
      <Field label="Reason" required>
        <input name="reason" value={reason} onChange={(e) => setReason(e.target.value)} />
      </Field>
      <label>
        <input type="checkbox" name="confirm" value="yes" required checked={approved} onChange={(e) => setApproved(e.target.checked)} /> I approve creating this policy version.
      </label>
      <div>
        <Button type="submit" loading={busy}>
          Create policy version
        </Button>
      </div>
    </Form>
  );
}

function ExperimentSummary({ experiment }: { experiment: J }) {
  const allocations = experimentAllocations(experiment);
  if (!allocations.length) return <p>No allocations have been configured.</p>;
  const sizes = experiment.sizes && typeof experiment.sizes === "object" && !Array.isArray(experiment.sizes) ? experiment.sizes : null;
  return (
    <>
      {sizes && (
        <p>
          <strong>{countText(Number(sizes.assignments ?? 0))} assignments.</strong> {countText(Number(sizes.holdout_assignments ?? 0))} holdout assignments.
        </p>
      )}
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Allocation</th>
              <th>Tier</th>
              <th>Share</th>
              <th>Assignments</th>
              <th>Expected</th>
              <th>Balanced</th>
            </tr>
          </thead>
          <tbody>
            {allocations.map((allocation, index) => (
              <tr key={index}>
                <td>{text(allocation, "name")}</td>
                <td>{text(allocation, "tier")}</td>
                <td>{allocationShare(allocation)}</td>
                <td>{"assignments" in allocation ? countText(Number(allocation.assignments)) : "—"}</td>
                <td>{"expected" in allocation ? cellText(allocation.expected) : "—"}</td>
                <td>{allocation.balanced === true ? "Yes" : "No"}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}

function DecisionsPanel({ ledger, decisions, scope }: { ledger: J | undefined; decisions: J[] | undefined; scope: string }) {
  const entries = ledgerEntries(ledger, scope);
  const counts = actionCounts(entries);
  const queue = a1Queue(entries, decisions ?? []);
  const hasDecisions = Array.isArray(decisions) && decisions.length > 0;
  if (counts.decisions === 0 && !hasDecisions) {
    return (
      <Panel title="Actions and A1 Queue">
        <p>No decisions have been produced yet. Run the solution to populate delivered, held and approval-pending actions.</p>
      </Panel>
    );
  }
  const latest = latestActions(entries);
  return (
    <Panel title="Actions and A1 Queue">
      <h3>Decision and Action Counts</h3>
      <dl>
        <dt>Decisions</dt>
        <dd>{countText(counts.decisions)}</dd>
        <dt>Actions</dt>
        <dd>{countText(counts.actions)}</dd>
        <dt>Held</dt>
        <dd>{countText(counts.held)}</dd>
      </dl>
      <h3>Latest Actions</h3>
      {latest.length === 0 ? (
        <p>No delivered actions are recorded for this solution.</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Entity</th>
                <th>Action</th>
                <th>Cell</th>
                <th>Score</th>
                <th>Model / Policy</th>
                <th>Recorded</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {latest.map((entry, index) => (
                <tr key={text(entry, "id") || index}>
                  <td>{text(entry, "subject")}</td>
                  <td>{text(entry, "action")}</td>
                  <td>{text(entry, "cell")}</td>
                  <td>{"score" in entry ? cellText(entry.score) : "—"}</td>
                  <td>
                    {text(entry, "model_version")}
                    <br />
                    {text(entry, "policy_version")}
                  </td>
                  <td>{text(entry, "recorded_at")}</td>
                  <td>
                    <OutcomeForm entry={entry} />
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <h3>A1 Queue</h3>
      {queue.length === 0 ? (
        <p>No held or approval-pending decisions are recorded for this solution.</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Entity</th>
                <th>Proposed Action</th>
                <th>Cell</th>
                <th>Score</th>
                <th>Governance</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {queue.map((row) => (
                <tr key={row.key}>
                  <td>{row.entity}</td>
                  <td>{row.proposed}</td>
                  <td>{row.cell}</td>
                  <td>{row.score}</td>
                  <td>{row.governance}</td>
                  <td>{row.decisionId ? <ApproveA1 decisionId={row.decisionId} entity={row.entity} action={row.proposed} /> : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <p className="sub">Approvals change the decision state; they do not silently deliver an action.</p>
    </Panel>
  );
}

/** Approve one A1 decision, after the person confirms it (the form's `confirm=yes`). */
function ApproveA1({ decisionId, entity, action }: { decisionId: string; entity: string; action: string }) {
  const scope = useScope();
  const toast = useToast();
  return (
    <ConfirmButton
      title="Approve this A1 decision?"
      confirmLabel="Approve A1"
      confirm={`Approving changes the decision for ${entity || "this entity"} (${action}) to approved. It does not deliver an action by itself.`}
      onConfirm={async () => {
        await wa(scope, "approve_decision", { decision_id: decisionId });
        toast("A1 decision approved");
      }}
    >
      Approve A1
    </ConfirmButton>
  );
}

function OutcomeForm({ entry }: { entry: J }) {
  const scope = useScope();
  const toast = useToast();
  const [outcome, setOutcome] = useState("");
  const [value, setValue] = useState("");
  const [currency, setCurrency] = useState(() => text(entry, "currency"));
  const [occurredAt, setOccurredAt] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const parsed = parseNumber(value);
      if (parsed === null) throw new Refusal("The outcome value must be a number.");
      await wa(scope, "record_outcome", { action_id: text(entry, "id"), outcome, value: parsed, currency, source: "manual", occurred_at: occurredAt });
      setOutcome("");
      setValue("");
      setOccurredAt("");
      toast("Outcome recorded");
    });
  return (
    <details>
      <summary>Record outcome</summary>
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="Outcome" required>
          <input name="outcome" value={outcome} onChange={(e) => setOutcome(e.target.value)} />
        </Field>
        <Field label="Value" required>
          <input name="value" type="number" step="any" value={value} onChange={(e) => setValue(e.target.value)} />
        </Field>
        <Field label="Currency" required>
          <input name="currency" value={currency} onChange={(e) => setCurrency(e.target.value)} />
        </Field>
        <Field label="Occurred at" required>
          <input name="occurred_at" placeholder="2026-09-28T12:00:00Z" value={occurredAt} onChange={(e) => setOccurredAt(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" loading={busy}>
            Record
          </Button>
        </div>
      </Form>
    </details>
  );
}

function MonitorSummary({ monitor }: { monitor: J }) {
  const guardrails: J[] = Array.isArray(monitor.guardrails) ? monitor.guardrails : [];
  return (
    <>
      <p>
        <RunBadge status={monitorHealthy(monitor) ? "within bounds" : "attention needed"} />
      </p>
      <dl>
        <dt>Input drift threshold</dt>
        <dd>{"psi_threshold" in monitor ? cellText(monitor.psi_threshold) : "—"}</dd>
        <dt>Calibration error threshold</dt>
        <dd>{"calibration_error_threshold" in monitor ? cellText(monitor.calibration_error_threshold) : "—"}</dd>
      </dl>
      {guardrails.length > 0 && (
        <>
          <h3>Guardrails</h3>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Guardrail</th>
                  <th>Observed</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                {guardrails.map((guardrail, index) => (
                  <tr key={index}>
                    <td>{text(guardrail.bound ?? {}, "name")}</td>
                    <td>{"value" in guardrail ? cellText(guardrail.value) : "Not observed"}</td>
                    <td>
                      <RunBadge status={guardrail.breached === true ? "failed" : "within bounds"} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </>
  );
}

/** The ROI readout: the claimed return, or the engine's reason it cannot be claimed yet. */
function RoiSummary({ readout }: { readout: J }) {
  const statement: J = readout.roi_statement && typeof readout.roi_statement === "object" ? readout.roi_statement : {};
  const model: J = statement.model && typeof statement.model === "object" ? statement.model : {};
  const currency = text(model, "currency");
  let reason = text(statement, "reason");
  if (!reason && readout.incrementality && typeof readout.incrementality === "object") reason = text(readout.incrementality, "reason");
  return (
    <>
      {statement.claimable === true ? (
        <p>
          <strong>{(Number(statement.return_on_investment ?? 0) * 100).toFixed(1)}% return on investment.</strong> Net value {currency} {cellText(statement.net_value ?? 0)} from
          gross value {currency} {cellText(statement.gross_value ?? 0)}.
        </p>
      ) : (
        <p>
          <strong>ROI cannot be claimed yet.</strong> {reason || "No measured reason is available."}
        </p>
      )}
      {readout.correctness_certificate && <Certificate certificate={readout.correctness_certificate} solution={text(readout, "solution")} />}
    </>
  );
}

function Certificate({ certificate, solution }: { certificate: J; solution: string }) {
  const { groups, hidden } = certificateGroups(certificate, solution);
  return (
    <>
      <h3>Correctness Certificate</h3>
      <p>
        <strong>Verdict:</strong> <RunBadge status={text(certificate, "verdict")} />
      </p>
      {groups.map(([group, checks]) => (
        <details key={group}>
          <summary>
            {group} ({checks.length} checks)
          </summary>
          <CheckTable checks={checks} runTone />
        </details>
      ))}
      {hidden.length > 0 && (
        <details>
          <summary>Show all checks ({hidden.length} not applicable)</summary>
          <CheckTable checks={hidden} runTone />
        </details>
      )}
    </>
  );
}

