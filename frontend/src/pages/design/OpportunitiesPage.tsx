/** Design → Opportunities, also at /design/discover (lake's discover_content): what Auto-Suggest
 * recommended for the workspace's data, a scan that runs in the background (`task=`, polled every 3
 * seconds while it works), project drafts made only on confirmation, and dismissal. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, useRead } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { Badge, Button, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath } from "./api";
import { findMethodology, groupOpportunities, taskIsActive, text, type J } from "./logic";
import { FormError, Markdown, Panel, Refusal, useDesignTrail, useSubmit } from "./parts";

const SCAN_MESSAGE = "What could this workspace build from its data?";

export default function OpportunitiesPage() {
  useDesignTrail({ label: "Opportunities", path: "/design/opportunities" }, [], "Opportunities");
  return (
    <Page
      eyebrow="Design"
      title="Opportunities"
      sub="What your data could be used for. Auto-Suggest reads the data your workspace can reach and recommends what to build, from Orama's solutions catalogue; it never changes your data. Turn an Opportunity into a project, or dismiss it."
    >
      <WorkspaceGate>
        <Opportunities />
      </WorkspaceGate>
    </Page>
  );
}

function Opportunities() {
  const scope = useScope();
  const [params, setParams] = useSearchParams();
  const taskId = params.get("task") ?? "";
  const task = useScopedRead<J>(taskId ? "get_agent_task" : null, { task_id: taskId }, {
    refetchInterval: (query) => (taskIsActive(text((query.state.data as J | undefined) ?? {}, "status")) ? 3000 : false),
  });
  const status = text(task.data ?? {}, "status");
  const scanning = taskIsActive(status);
  // With the scan's task, a finished scan is saved into Opportunities the first time it is read.
  const listed = useScopedRead<{ opportunities?: J[] }>("list_opportunities", { task_id: taskId || undefined }, { refetchInterval: scanning ? 3000 : false });
  const methodologies = useWq<J>("list_methodologies", {}, { retry: false });
  const library = useRead<{ agents?: J[] }>("list_agent_library", {});

  if (listed.isLoading || library.isLoading) return <Skeleton />;

  const running = (library.data?.agents ?? []).some((a) => text(a, "key") === "agent-autosuggest" && a.deployed === true);
  const rows: J[] = Array.isArray(listed.data?.opportunities) ? listed.data!.opportunities! : [];
  const groups = groupOpportunities(rows);
  // Absent on an engine that predates the registry: then no badge.
  const registry = !methodologies.isError && Array.isArray(methodologies.data?.methodologies) ? methodologies.data!.methodologies : null;

  return (
    <>
      {scanning && (
        <section className="panel notice" role="status">
          <p>
            <strong>Scanning your workspace…</strong> Auto-Suggest is reading your datasets and matching them to the solutions catalogue. You can leave this page; the
            Opportunities are kept when it finishes. This page checks again every 3 seconds.
          </p>
        </section>
      )}
      {status === "failed" && (
        <p className="error" role="alert">
          The scan did not finish: {text(task.data ?? {}, "message")}
        </p>
      )}
      {task.isError && <ErrorBlock error={task.error} what="the scan" />}
      {library.isError ? (
        <ErrorBlock error={library.error} retry={() => void library.refetch()} what="the Agent Library" />
      ) : !running ? (
        <div className="panel empty">Auto-Suggest is not running on this lake, so a new scan is not available.</div>
      ) : (
        !scanning && <Scan lastScan={groups.lastScan} customerId={scope.customerId} workspaceId={scope.workspaceId} onTask={(id) => setParams(new URLSearchParams({ task: id }))} />
      )}
      {listed.isError ? (
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="Opportunities" />
      ) : (
        <>
          {rows.length === 0 && !scanning && <div className="panel empty">No Opportunities yet. Scan your workspace to see what your data could be used for.</div>}
          {(groups.fresh.length > 0 || groups.earlier.length > 0) && <Recommended fresh={groups.fresh} earlier={groups.earlier} registry={registry} />}
          {groups.made.length > 0 && <Made rows={groups.made} />}
          {groups.later.length > 0 && (
            <Panel title="Not Yet">
              <p className="sub">What your data does not support yet, and why.</p>
              <ul>
                {groups.later.map((row) => (
                  <li key={text(row, "id")}>
                    <strong>{text(row, "intent")}</strong>: {text(row, "why")}
                  </li>
                ))}
              </ul>
            </Panel>
          )}
          {groups.dismissed.length > 0 && (
            <details className="panel">
              <summary>Dismissed</summary>
              <ul>
                {groups.dismissed.map((row) => (
                  <li key={text(row, "id")}>{text(row, "intent")}</li>
                ))}
              </ul>
            </details>
          )}
        </>
      )}
    </>
  );
}

function Scan({ lastScan, customerId, workspaceId, onTask }: { lastScan: string; customerId: string; workspaceId: string; onTask: (id: string) => void }) {
  const { busy, error, run } = useSubmit();
  const scan = () =>
    run(async () => {
      const answer = await act<J>("agent_turn", { customer_id: customerId, workspace_id: workspaceId, agent: "agent-autosuggest", kind: "discover", message: SCAN_MESSAGE });
      onTask(text(answer, "task_id"));
    });
  return (
    <Panel title="Scan Your Workspace">
      <p>
        {lastScan
          ? `Last scanned ${lastScan.slice(0, 16)} UTC. Scan again after your data changes; earlier Opportunities stay.`
          : "Auto-Suggest reads your workspace's datasets and their columns and recommends what to build. It runs in the background and changes nothing."}
      </p>
      <FormError error={error} />
      <Button variant="primary" loading={busy} onClick={() => void scan()}>
        {lastScan ? "Scan again" : "Scan my workspace"}
      </Button>
    </Panel>
  );
}

/** The newest scan's recommendations, earlier unacted ones collapsed, picked into project drafts on
 * confirmation. */
function Recommended({ fresh, earlier, registry }: { fresh: J[]; earlier: J[]; registry: J[] | null }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [picked, setPicked] = useState<Set<string>>(new Set());
  const [confirmed, setConfirmed] = useState(false);
  const { busy, error, run } = useSubmit();
  const toggle = (id: string, on: boolean) =>
    setPicked((current) => {
      const next = new Set(current);
      if (on) next.add(id);
      else next.delete(id);
      return next;
    });
  const submit = () =>
    run(async () => {
      if (!confirmed) throw new Refusal("Confirm to create the drafts.");
      if (!picked.size) throw new Refusal("Choose at least one Opportunity.");
      await act("create_projects_from_opportunities", { customer_id: scope.customerId, workspace_id: scope.workspaceId, opportunity_ids: [...picked] });
      navigate(ws("/design/projects"));
    });
  const card = (row: J) => <OpportunityCard key={text(row, "id")} row={row} registry={registry} picked={picked.has(text(row, "id"))} onPick={toggle} />;
  return (
    <Panel title="Recommended">
      <form
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        {fresh.length ? fresh.map(card) : <p className="sub">The latest scan recommended nothing new.</p>}
        {earlier.length > 0 && (
          <details className="earlier-scans">
            <summary>Earlier scans ({earlier.length})</summary>
            <p className="sub">What earlier scans recommended and nobody has acted on. The latest scan above replaces them; you can still pick or dismiss one.</p>
            {earlier.map(card)}
          </details>
        )}
        <label>
          <input type="checkbox" name="confirm" value="yes" checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} /> Create a project draft for each selected
          Opportunity. Nothing runs until you approve a plan.
        </label>
        <FormError error={error} />
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Create project drafts
          </Button>
        </div>
      </form>
    </Panel>
  );
}

function OpportunityCard({ row, registry, picked, onPick }: { row: J; registry: J[] | null; picked: boolean; onPick: (id: string, on: boolean) => void }) {
  const scope = useScope();
  const toast = useToast();
  const { busy, error, run } = useSubmit();
  const id = text(row, "id");
  const detail: J = row.detail && typeof row.detail === "object" ? row.detail : {};
  const entry = registry ? findMethodology(registry, text(row, "methodology"), text(row, "solution")) : null;
  const dismiss = () =>
    run(async () => {
      await act("dismiss_opportunity", { customer_id: scope.customerId, workspace_id: scope.workspaceId, opportunity_id: id });
      toast("Opportunity dismissed");
    });
  return (
    <article className="panel ask-proposal">
      <label className="opportunity-pick">
        <input type="checkbox" name="pick" value={id} checked={picked} onChange={(e) => onPick(id, e.target.checked)} /> <span>{text(row, "intent")}</span>
      </label>
      <p className="sub">
        {text(row, "solution") ? `Catalogue solution ${text(row, "solution")} · ` : ""}
        {text(row, "methodology")}
      </p>
      {entry && (
        <p>
          {entry.runnable === true ? (
            <Badge tone="success">{`Runs today: ${text(entry, "name")} (${text(entry, "id")})`}</Badge>
          ) : (
            <Badge tone="warning">{`Needs ${text(entry, "id")}, not supported yet${text(entry, "issue") ? ` (${text(entry, "issue")})` : ""}`}</Badge>
          )}
        </p>
      )}
      {/* Auto-Suggest writes inline Markdown (`table` names). */}
      {text(row, "why") && <Markdown>{text(row, "why")}</Markdown>}
      {text(detail, "business_action") && <Markdown>{`**What it changes:** ${text(detail, "business_action")}`}</Markdown>}
      <FormError error={error} />
      <p>
        <Button loading={busy} onClick={() => void dismiss()}>
          Dismiss
        </Button>
      </p>
    </article>
  );
}

function Made({ rows }: { rows: J[] }) {
  const ws = useWsPath();
  return (
    <Panel title="Made into Projects">
      <ul>
        {rows.map((row) => (
          <li key={text(row, "id")}>
            {text(row, "intent")} → <Link to={ws(`/design/project?${qs({ name: text(row, "project") })}`)}>{text(row, "project")}</Link>
          </li>
        ))}
      </ul>
    </Panel>
  );
}
