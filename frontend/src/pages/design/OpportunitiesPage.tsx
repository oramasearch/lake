/** Design → Opportunities, also at /design/discover (lake's discover_content): what Auto-Suggest
 * recommended for the workspace's data, a scan that runs in the background (`task=`, polled every 3
 * seconds while it works), project drafts made only on confirmation, and dismissal. */
import { type ReactNode, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Check, FolderPlus, Lightbulb, ScanSearch, X } from "lucide-react";
import { act, useRead } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, Chip, Empty, ErrorBlock, Grid, Page, Section, Skeleton, Stat, StatRow } from "@/lib/ui";
import { relative } from "@/lib/format";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { MethodologyIcon, SolutionIcon } from "@/components/CatalogueIcon";
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
      sub="What your data could be used for. Auto-Suggest reads the data your workspace can reach and recommends what to build from Orama's solutions catalogue; it never changes your data."
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
  const scanButton = (label?: string) => (
    <ScanButton customerId={scope.customerId} workspaceId={scope.workspaceId} label={label ?? (groups.lastScan ? "Scan again" : "Scan my workspace")} onTask={(id) => setParams(new URLSearchParams({ task: id }))} />
  );

  return (
    <div className="space-y-4">
      {scanning ? (
        <Banner title="Scanning your workspace…">
          Auto-Suggest is reading your datasets and matching them to the solutions catalogue. You can leave this page; the Opportunities are kept when it finishes. This page
          checks again every 3 seconds.
        </Banner>
      ) : library.isError ? (
        <ErrorBlock error={library.error} retry={() => void library.refetch()} what="the Agent Library" />
      ) : !running ? (
        <Banner tone="quiet">Auto-Suggest is not running on this Lake, so a new scan is not available.</Banner>
      ) : (
        <Banner
          title={groups.lastScan ? `Last scanned ${relative(groups.lastScan)}` : "Scan your workspace"}
          action={scanButton()}
        >
          {groups.lastScan ? (
            <span title={`${groups.lastScan.slice(0, 16)} UTC`}>Scan again after your data changes; earlier Opportunities stay.</span>
          ) : (
            "Auto-Suggest reads your workspace's datasets and their columns and recommends what to build. It runs in the background and changes nothing."
          )}
        </Banner>
      )}
      {status === "failed" && <Banner tone="warn" title="The scan did not finish">{text(task.data ?? {}, "message")}</Banner>}
      {task.isError && <ErrorBlock error={task.error} what="the scan" />}
      {listed.isError ? (
        <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="Opportunities" />
      ) : (
        <>
          {rows.length > 0 && (
            <StatRow cols={4}>
              <Stat label="Recommended" value={groups.fresh.length + groups.earlier.length} icon={Lightbulb} sub={groups.earlier.length ? `${groups.earlier.length} from earlier scans` : "from the latest scan"} />
              <Stat label="Made into projects" value={groups.made.length} tone={groups.made.length ? "ok" : undefined} />
              <Stat label="Not yet" value={groups.later.length} sub="the data does not support yet" />
              <Stat label="Dismissed" value={groups.dismissed.length} />
            </StatRow>
          )}
          {rows.length === 0 && !scanning && (
            <Empty icon={Lightbulb} title="No Opportunities Yet" action={running && !library.isError ? scanButton("Scan my workspace") : undefined}>
              Scan your workspace to see what your data could be used for.
            </Empty>
          )}
          {(groups.fresh.length > 0 || groups.earlier.length > 0) && <Recommended fresh={groups.fresh} earlier={groups.earlier} registry={registry} />}
          {groups.made.length > 0 && <Made rows={groups.made} />}
          {groups.later.length > 0 && (
            <Panel title="Not Yet" label="What your data does not support yet, and why">
              <ul className="m-0 space-y-1.5 pl-4 text-[13px]">
                {groups.later.map((row) => (
                  <li key={text(row, "id")}>
                    <strong>{text(row, "intent")}</strong>: <span className="text-ink-2">{text(row, "why")}</span>
                  </li>
                ))}
              </ul>
            </Panel>
          )}
          {groups.dismissed.length > 0 && (
            <Card tone="quiet">
              <details>
                <summary className="cursor-pointer text-[13px] font-semibold">Dismissed ({groups.dismissed.length})</summary>
                <ul className="mb-0 mt-2 space-y-1 pl-4 text-[13px] text-ink-2">
                  {groups.dismissed.map((row) => (
                    <li key={text(row, "id")}>{text(row, "intent")}</li>
                  ))}
                </ul>
              </details>
            </Card>
          )}
        </>
      )}
    </div>
  );
}

function ScanButton({ customerId, workspaceId, label, onTask }: { customerId: string; workspaceId: string; label: string; onTask: (id: string) => void }) {
  const { busy, error, run } = useSubmit();
  const scan = () =>
    run(async () => {
      const answer = await act<J>("agent_turn", { customer_id: customerId, workspace_id: workspaceId, agent: "agent-autosuggest", kind: "discover", message: SCAN_MESSAGE });
      onTask(text(answer, "task_id"));
    });
  return (
    <div className="flex flex-col items-end gap-1">
      <Button variant="primary" loading={busy} onClick={() => void scan()}>
        <ScanSearch size={14} aria-hidden /> {label}
      </Button>
      <FormError error={error} />
    </div>
  );
}

/** The newest scan's recommendations, earlier unacted ones collapsed. Each card makes its own
 * project draft or is dismissed; several picked ones become drafts together, on confirmation. */
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
  const create = async (ids: string[]) => {
    await act("create_projects_from_opportunities", { customer_id: scope.customerId, workspace_id: scope.workspaceId, opportunity_ids: ids });
    navigate(ws("/design/projects"));
  };
  const submit = () =>
    run(async () => {
      if (!confirmed) throw new Refusal("Confirm to create the drafts.");
      if (!picked.size) throw new Refusal("Choose at least one Opportunity.");
      await create([...picked]);
    });
  const cards = (list: J[]): ReactNode => (
    <Grid className="grid-cols-1 gap-3 lg:grid-cols-2">
      {list.map((row) => (
        <OpportunityCard key={text(row, "id")} row={row} registry={registry} picked={picked.has(text(row, "id"))} onPick={toggle} onCreate={create} />
      ))}
    </Grid>
  );
  return (
    <Section title="Recommended">
      <form
        className="space-y-3"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        {fresh.length ? cards(fresh) : <p className="m-0 text-[13px] text-ink-2">The latest scan recommended nothing new.</p>}
        {earlier.length > 0 && (
          <details className="earlier-scans">
            <summary className="cursor-pointer text-[13px] font-semibold">Earlier scans ({earlier.length})</summary>
            <p className="text-[13px] text-ink-2">What earlier scans recommended and nobody has acted on. The latest scan above replaces them; you can still pick or dismiss one.</p>
            {cards(earlier)}
          </details>
        )}
        <Card tone="active">
          <div className="flex flex-wrap items-center gap-3">
            <label className="flex min-w-[14rem] flex-1 items-start gap-2 text-[13px]">
              <input type="checkbox" name="confirm" value="yes" checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} />
              <span>Create a project draft for each selected Opportunity. Nothing runs until you approve a plan.</span>
            </label>
            <Button type="submit" variant="primary" loading={busy}>
              <FolderPlus size={14} aria-hidden /> Create project drafts{picked.size ? ` (${picked.size})` : ""}
            </Button>
          </div>
          <FormError error={error} />
        </Card>
      </form>
    </Section>
  );
}

function OpportunityCard({
  row,
  registry,
  picked,
  onPick,
  onCreate,
}: {
  row: J;
  registry: J[] | null;
  picked: boolean;
  onPick: (id: string, on: boolean) => void;
  onCreate: (ids: string[]) => Promise<void>;
}) {
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
    <Card
      tone={picked ? "active" : "default"}
      icon={
        // The catalogue solution it recommends, else the methodology it needs (lake#639).
        text(row, "solution") ? (
          <SolutionIcon entry={{ id: text(row, "solution"), methodologies: text(row, "methodology") ? [text(row, "methodology")] : [] }} size={32} />
        ) : text(row, "methodology") ? (
          <MethodologyIcon entry={entry ?? { id: text(row, "methodology") }} size={32} />
        ) : undefined
      }
      label={[text(row, "solution") ? `Catalogue solution ${text(row, "solution")}` : "", text(row, "methodology")].filter(Boolean).join(" · ") || undefined}
      title={
        <label className="opportunity-pick flex cursor-pointer items-start gap-2">
          <input type="checkbox" name="pick" value={id} checked={picked} onChange={(e) => onPick(id, e.target.checked)} className="mt-1" /> <span>{text(row, "intent")}</span>
        </label>
      }
      footer={
        <div className="flex flex-wrap items-center justify-end gap-2">
          <FormError error={error} />
          <Button variant="ghost" loading={busy} onClick={() => void dismiss()}>
            <X size={14} aria-hidden /> Dismiss
          </Button>
          <ConfirmButton
            variant="primary"
            title="Create a project draft?"
            confirmLabel="Create project draft"
            confirm={`A project draft is made from “${text(row, "intent")}”. Nothing runs until you approve a plan.`}
            onConfirm={() => onCreate([id])}
          >
            <Check size={14} aria-hidden /> Create project
          </ConfirmButton>
        </div>
      }
    >
      {entry && (
        <div className="mb-2">
          {entry.runnable === true ? (
            <Chip tone="ok">{`Runs today: ${text(entry, "name")} (${text(entry, "id")})`}</Chip>
          ) : (
            <Chip tone="warn">{`Needs ${text(entry, "id")}, not supported yet${text(entry, "issue") ? ` (${text(entry, "issue")})` : ""}`}</Chip>
          )}
        </div>
      )}
      {/* Auto-Suggest writes inline Markdown (`table` names). */}
      <div className="text-[13px] text-ink-2">
        {text(row, "why") && <Markdown>{text(row, "why")}</Markdown>}
        {text(detail, "business_action") && <Markdown>{`**What it changes:** ${text(detail, "business_action")}`}</Markdown>}
      </div>
    </Card>
  );
}

function Made({ rows }: { rows: J[] }) {
  const ws = useWsPath();
  return (
    <Panel title="Made into Projects">
      <ul className="m-0 divide-y divide-hairline pl-0">
        {rows.map((row) => (
          <li key={text(row, "id")} className="flex list-none flex-wrap items-center justify-between gap-2 py-2 text-[13px]">
            <span className="min-w-0 text-ink-2">{text(row, "intent")}</span>
            <Link to={ws(`/design/project?${qs({ name: text(row, "project") })}`)} className="font-semibold">
              {text(row, "project")} →
            </Link>
          </li>
        ))}
      </ul>
    </Panel>
  );
}
