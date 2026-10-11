/** A solution project's panels on the project page (lake's active_solution_content and
 * solution_proposal_content): its stages and Run, the decision policy, the experiment and holdout,
 * the actions and A1 queue, the monitor and the ROI readout; or, before it exists, the Project
 * Agent's foundation proposal to confirm. Every confirmation the form handlers enforced is enforced
 * here before the call. */
import { useState } from "react";
import { act } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, DataTable, ErrorBlock, Field, Form, KeyValue, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
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
import { CheckTable, FormError, JsonDisclosure, Panel, Refusal, RunBadge, When, useSubmit } from "./parts";

/* ------------------------------------------------------------------ the foundation proposal */

/** The `solution:<project>` task: working, failed, or its proposal with the confirm form. */
export function ProposalContent({ project, task }: { project: string; task: J }) {
  const status = text(task, "status");
  if (status === "pending" || status === "running") {
    return (
      <Banner title="Foundation proposal: the Project Agent is reading the dataset…">
        It is proposing the snapshot boundary, column roles, methodology inputs and policy predicates. Nothing is created until you confirm. This page checks again every 3
        seconds.
        {text(task, "message") && <div className="mt-1">{text(task, "message")}</div>}
      </Banner>
    );
  }
  if (status === "failed") {
    return <Banner tone="warn">The Project Agent could not propose the foundation: {text(task, "message")}</Banner>;
  }
  const proposal = proposedFoundation(task);
  if (!Object.keys(proposal).length) {
    return status === "succeeded" ? <Banner tone="warn">The Project Agent returned no foundation proposal.</Banner> : null;
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
    <Panel title="Foundation Proposal" label="Review before anything is created" tone="active">
      <p className="mt-0 text-[13px] text-ink-2">Review and edit the exact foundation sent to the engine. Delivery and economics are your inputs from Start.</p>
      {missing && (
        <Banner tone="warn" title="Actions will remain blocked.">
          The dataset is missing these exclusion predicates: {missing}. They are not treated as false.
        </Banner>
      )}
      <Form onSubmit={submit}>
        <FormError error={error} />
        <Field label="Foundation (JSON)" required>
          <textarea name="foundation" rows={24} className="font-mono text-[12.5px]" value={foundation} onChange={(e) => setFoundation(e.target.value)} />
        </Field>
        <label className="flex items-start gap-2 text-[13px]">
          <input type="checkbox" name="confirm" value="yes" required checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} />
          <span>I reviewed this snapshot, these roles, predicates, delivery settings and economics.</span>
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
      <Panel title="Solution Stages" label={solution.graph}>
        <DataTable
          rows={solution.stages}
          caption="Solution stages"
          rowKey={(stage) => stage.id}
          columns={[
            { key: "stage", label: "Stage", render: (stage) => <span className="font-mono">{cellText(stage.stage)}</span> },
            { key: "name", label: "Name", render: (stage) => <span className="font-semibold">{stage.name}</span> },
            { key: "kind", label: "Kind", render: (stage) => <span className="text-ink-2">{stage.kind}</span> },
          ]}
        />
        <div className="mt-3">
          {missing.length === 0 ? (
            <RunSolution project={project} graph={solution.graph} />
          ) : (
            <Banner tone="warn">Run is blocked because an exclusion predicate is missing. Missing evidence is never treated as false.</Banner>
          )}
        </div>
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
      <KeyValue
        rows={[
          ["Version", <span key="v" className="font-mono">{cellText(policy.current_version ?? 0)}</span>],
          ["Autonomy", text(config, "autonomy") || "—"],
          ["Score threshold", <span key="t" className="font-mono">{config && "threshold" in config ? cellText(config.threshold) : "—"}</span>],
        ]}
      />
      {tiers.length > 0 && (
        <>
          <h3>Tiers and Thresholds</h3>
          <DataTable
            rows={tiers}
            caption="Tiers and thresholds"
            columns={[
              { key: "name", label: "Tier", render: (tier) => text(tier, "name") },
              { key: "minimum_score", label: "Minimum Score", numeric: true, render: (tier) => <span className="font-mono">{cellText(tier.minimum_score ?? 0)}</span> },
              { key: "action", label: "Action", render: (tier) => text(tier, "action") },
            ]}
          />
        </>
      )}
      <JsonDisclosure value={policy} />
      <details className="mt-2">
        <summary className="cursor-pointer text-[12.5px] text-ink-2">Propose a New Policy Version</summary>
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
        <textarea name="config" rows={16} className="font-mono text-[12.5px]" value={value} onChange={(e) => setValue(e.target.value)} />
      </Field>
      <Field label="Reason" required>
        <input name="reason" value={reason} onChange={(e) => setReason(e.target.value)} />
      </Field>
      <label className="flex items-start gap-2 text-[13px]">
        <input type="checkbox" name="confirm" value="yes" required checked={approved} onChange={(e) => setApproved(e.target.checked)} />
        <span>I approve creating this policy version.</span>
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
  if (!allocations.length) return <p className="m-0 text-[13px] text-ink-2">No allocations have been configured.</p>;
  const sizes = experiment.sizes && typeof experiment.sizes === "object" && !Array.isArray(experiment.sizes) ? experiment.sizes : null;
  return (
    <>
      {sizes && (
        <StatRow cols={2}>
          <Stat label="Assignments" value={countText(Number(sizes.assignments ?? 0))} />
          <Stat label="Holdout assignments" value={countText(Number(sizes.holdout_assignments ?? 0))} />
        </StatRow>
      )}
      <DataTable
        rows={allocations}
        caption="Allocations"
        columns={[
          { key: "name", label: "Allocation", render: (a) => <span className="font-semibold">{text(a, "name")}</span> },
          { key: "tier", label: "Tier", render: (a) => text(a, "tier") },
          { key: "share", label: "Share", numeric: true, render: (a) => <span className="font-mono">{allocationShare(a)}</span> },
          { key: "assignments", label: "Assignments", numeric: true, render: (a) => <span className="font-mono">{"assignments" in a ? countText(Number(a.assignments)) : "—"}</span> },
          { key: "expected", label: "Expected", numeric: true, render: (a) => <span className="font-mono">{"expected" in a ? cellText(a.expected) : "—"}</span> },
          { key: "balanced", label: "Balanced", render: (a) => (a.balanced === true ? <Status value="Yes" tone="success" /> : <Status value="No" tone="warning" />) },
        ]}
      />
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
        <p className="m-0 text-[13px] text-ink-2">No decisions have been produced yet. Run the solution to populate delivered, held and approval-pending actions.</p>
      </Panel>
    );
  }
  const latest = latestActions(entries);
  return (
    <Panel title="Actions and A1 Queue" footer="Approvals change the decision state; they do not silently deliver an action.">
      <StatRow cols={3}>
        <Stat label="Decisions" value={countText(counts.decisions)} />
        <Stat label="Actions" value={countText(counts.actions)} />
        <Stat label="Held" value={countText(counts.held)} />
      </StatRow>
      <h3>Latest Actions</h3>
      <DataTable
        rows={latest}
        caption="Latest actions"
        rowKey={(entry, index) => text(entry, "id") || String(index)}
        empty={<p className="m-0 text-[13px] text-ink-2">No delivered actions are recorded for this solution.</p>}
        columns={[
          { key: "subject", label: "Entity", render: (entry) => <span className="font-semibold">{text(entry, "subject")}</span> },
          { key: "action", label: "Action", render: (entry) => text(entry, "action") },
          { key: "cell", label: "Cell", render: (entry) => text(entry, "cell") },
          { key: "score", label: "Score", numeric: true, render: (entry) => <span className="font-mono">{"score" in entry ? cellText(entry.score) : "—"}</span> },
          {
            key: "versions",
            label: "Model / Policy",
            render: (entry) => (
              <span className="font-mono text-[11.5px] text-ink-2">
                {text(entry, "model_version")}
                <br />
                {text(entry, "policy_version")}
              </span>
            ),
          },
          { key: "recorded_at", label: "Recorded", render: (entry) => <When value={text(entry, "recorded_at")} /> },
          { key: "outcome", label: "", render: (entry) => <OutcomeForm entry={entry} /> },
        ]}
      />
      <h3>A1 Queue</h3>
      <DataTable
        rows={queue}
        caption="A1 queue"
        rowKey={(row) => row.key}
        empty={<p className="m-0 text-[13px] text-ink-2">No held or approval-pending decisions are recorded for this solution.</p>}
        columns={[
          { key: "entity", label: "Entity", render: (row) => <span className="font-semibold">{row.entity}</span> },
          { key: "proposed", label: "Proposed Action", render: (row) => row.proposed },
          { key: "cell", label: "Cell", render: (row) => row.cell },
          { key: "score", label: "Score", numeric: true, render: (row) => <span className="font-mono">{row.score}</span> },
          { key: "governance", label: "Governance", render: (row) => <Status value={row.governance} /> },
          { key: "approve", label: "", render: (row) => (row.decisionId ? <ApproveA1 decisionId={row.decisionId} entity={row.entity} action={row.proposed} /> : "—") },
        ]}
      />
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
      <summary className="cursor-pointer whitespace-nowrap text-[12.5px] text-action">Record outcome</summary>
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
      <KeyValue
        className="mb-3"
        rows={[
          ["Health", <RunBadge key="h" status={monitorHealthy(monitor) ? "within bounds" : "attention needed"} />],
          ["Input drift threshold", <span key="p" className="font-mono">{"psi_threshold" in monitor ? cellText(monitor.psi_threshold) : "—"}</span>],
          ["Calibration error threshold", <span key="c" className="font-mono">{"calibration_error_threshold" in monitor ? cellText(monitor.calibration_error_threshold) : "—"}</span>],
        ]}
      />
      {guardrails.length > 0 && (
        <>
          <h3>Guardrails</h3>
          <DataTable
            rows={guardrails}
            caption="Guardrails"
            columns={[
              { key: "name", label: "Guardrail", render: (g) => text(g.bound ?? {}, "name") },
              { key: "value", label: "Observed", numeric: true, render: (g) => <span className="font-mono">{"value" in g ? cellText(g.value) : "Not observed"}</span> },
              { key: "status", label: "Status", render: (g) => <RunBadge status={g.breached === true ? "failed" : "within bounds"} /> },
            ]}
          />
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
        <StatRow cols={3}>
          <Stat label="Return on investment" value={`${(Number(statement.return_on_investment ?? 0) * 100).toFixed(1)}%`} tone="ok" />
          <Stat label="Net value" value={`${currency} ${cellText(statement.net_value ?? 0)}`} />
          <Stat label="Gross value" value={`${currency} ${cellText(statement.gross_value ?? 0)}`} />
        </StatRow>
      ) : (
        <Banner tone="quiet" title="ROI cannot be claimed yet.">
          {reason || "No measured reason is available."}
        </Banner>
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
      <p className="text-[13px]">
        <span className="text-ink-3">Verdict </span>
        <RunBadge status={text(certificate, "verdict")} />
      </p>
      {groups.map(([group, checks]) => (
        <details key={group} className="mt-1">
          <summary className="cursor-pointer text-[13px]">
            {group} ({checks.length} checks)
          </summary>
          <CheckTable checks={checks} runTone />
        </details>
      ))}
      {hidden.length > 0 && (
        <details className="mt-1">
          <summary className="cursor-pointer text-[13px] text-ink-2">Show all checks ({hidden.length} not applicable)</summary>
          <CheckTable checks={hidden} runTone />
        </details>
      )}
    </>
  );
}

