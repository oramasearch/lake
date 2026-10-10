/** The pieces several Design pages share, in the kit's look: the card panel, the Runs table, status
 * chips, the gate checks, a JSON disclosure, Markdown, the pipeline graph drawn in the browser,
 * tabs kept in the address, and the workspace breadcrumb. */
import { type ReactNode, useCallback, useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Card, DataTable, Status, cx } from "@/lib/ui";
import { relative } from "@/lib/format";
import { useTrail, entityCrumb, type Crumb } from "@/lib/trail";
import { useWsPath, qs } from "./api";
import { LAYOUT, layoutGraph, runIsActive, runStatusTone, runsBadge, text, verdictTone, whenUtc, type Graph, type J } from "./logic";

/** A titled card (the kit's Card) that keeps an anchor `id` and a `role`, so `#run` and
 * `#proposal` still land on it. */
export function Panel({
  title,
  label,
  actions,
  footer,
  tone,
  children,
  id,
  className,
  role,
}: {
  title?: ReactNode;
  label?: ReactNode;
  actions?: ReactNode;
  footer?: ReactNode;
  tone?: "default" | "exception" | "active" | "quiet";
  children?: ReactNode;
  id?: string;
  className?: string;
  role?: string;
}) {
  return (
    <section id={id} role={role} className={cx("min-w-0 scroll-mt-4", className)}>
      <Card title={title} label={label} actions={actions} footer={footer} tone={tone}>
        {children}
      </Card>
    </section>
  );
}

export { Markdown } from "@/lib/markdown";

/** A run, check or monitor status as a state chip (run_status_badge's words and tones). */
export function RunBadge({ status }: { status: string }) {
  return <Status value={status || "unknown"} tone={runStatusTone(status)} />;
}

/** Whether a methodology or solution runs here (runs_badge_of). */
export function RunsBadge({ entry }: { entry: J }) {
  const b = runsBadge(entry);
  return <Status value={b.label} tone={b.tone} />;
}

/** An identifier in mono, abbreviated as lake did (12 characters … last 7), whole on hover. */
export function Identifier({ value }: { value: string }) {
  const short = value.length <= 20 ? value : `${value.slice(0, 12)}…${value.slice(-7)}`;
  return (
    <code className="identifier" title={value}>
      {short}
    </code>
  );
}

export function JsonDisclosure({ value }: { value: unknown }) {
  return (
    <details className="mt-2">
      <summary className="cursor-pointer text-[12.5px] text-ink-2">View JSON</summary>
      <pre className="mt-2 max-h-96 overflow-auto">{JSON.stringify(value, null, 2)}</pre>
    </details>
  );
}

/** A timestamp as "3h ago", the exact UTC time on hover. */
export function When({ value }: { value: string }) {
  if (!value) return <span className="text-ink-3">—</span>;
  return (
    <span title={whenUtc(value)} className="whitespace-nowrap">
      {relative(value)}
    </span>
  );
}

/** Tabs that live in the address as `?tab=<id>`, keeping every other parameter of the page. The
 * `fallback` tab (the first by default) is the one shown without `tab=`, so its link leaves it out. */
export function QueryTabs<T extends string>({ tabs, value, fallback }: { tabs: { id: T; label: ReactNode; count?: number }[]; value: T; fallback?: T }) {
  const [params] = useSearchParams();
  const base = fallback ?? tabs[0]?.id;
  const href = (id: T) => {
    const next = new URLSearchParams(params);
    if (id === base) next.delete("tab");
    else next.set("tab", id);
    return `?${next.toString()}`;
  };
  return (
    <nav className="tabs" aria-label="Sections">
      {tabs.map((tab) => (
        <Link key={tab.id} to={href(tab.id)} replace aria-current={tab.id === value ? "page" : undefined}>
          {tab.label}
          {tab.count !== undefined && <span className="tab-count">{tab.count}</span>}
        </Link>
      ))}
    </nav>
  );
}

/** The `?tab=` a page shows: one of `ids`, else `fallback`. */
export function useQueryTab<T extends string>(ids: readonly T[], fallback: T): T {
  const [params] = useSearchParams();
  const asked = params.get("tab") as T | null;
  return asked && ids.includes(asked) ? asked : fallback;
}

/** The runs of a project or solution (solution_runs_content). The page polls every 3 seconds
 * while a run is active; the footer says so. */
export function RunsPanel({ runs, title = "Runs" }: { runs: J[]; title?: ReactNode }) {
  const ws = useWsPath();
  const active = runs.some(runIsActive);
  return (
    <Panel title={title} footer={active ? "This page checks again every 3 seconds while a run is active." : undefined}>
      <DataTable<J>
        rows={runs}
        what="runs"
        caption="Runs"
        empty={<p className="m-0 text-[13px] text-ink-2">No runs yet.</p>}
        rowKey={(run, index) => text(run, "id") || String(index)}
        columns={[
          {
            key: "id",
            label: "Run",
            render: (run) => (
              <Link to={ws(`/train/run?${qs({ id: text(run, "id") })}`)} title="Open full run">
                <Identifier value={text(run, "id")} />
              </Link>
            ),
          },
          { key: "status", label: "Status", sort: (run) => text(run, "status"), render: (run) => <RunBadge status={text(run, "status")} /> },
          { key: "stage", label: "Stage Reached", render: (run) => text(run, "stage") || <span className="text-ink-3">—</span> },
          { key: "created", label: "Started", sort: (run) => text(run, "created"), render: (run) => <When value={text(run, "created")} /> },
          {
            key: "error",
            label: "Error",
            render: (run) => (text(run, "error") ? <span className="text-bad">{text(run, "error")}</span> : <span className="text-ink-3">—</span>),
          },
        ]}
      />
    </Panel>
  );
}

/** Gate checks, from gate or from deploy's gate (gate_panel). */
export function GatePanel({ gate, heading }: { gate: J; heading: string }) {
  const verdict = text(gate, "verdict");
  const blocked = gate?.block_deploy === true;
  const checks: J[] = Array.isArray(gate?.checks) ? gate.checks : [];
  return (
    <Panel title={heading} tone={blocked ? "exception" : "active"} role="status" actions={<Status value={verdict || (blocked ? "fail" : "pass")} tone={verdictTone(verdict || (blocked ? "fail" : "pass"))} />}>
      <p className={cx("mt-0 text-[13px]", blocked ? "text-bad" : "text-ink-2")}>{blocked ? "It cannot run or deploy until the failing checks pass." : "It may run and deploy."}</p>
      {checks.length > 0 && <CheckTable checks={checks} />}
    </Panel>
  );
}

export function CheckTable({ checks, runTone }: { checks: J[]; runTone?: boolean }) {
  return (
    <DataTable<J>
      rows={checks}
      caption="Checks"
      columns={[
        { key: "name", label: "Check", render: (check) => <span className="font-mono text-[12.5px]">{text(check, "name")}</span> },
        {
          key: "status",
          label: "Status",
          render: (check) => {
            const status = text(check, "status");
            return runTone ? <RunBadge status={status} /> : <Status value={status} tone={verdictTone(status)} />;
          },
        },
        { key: "reason", label: "Why", render: (check) => <span className="text-ink-2">{text(check, "reason")}</span> },
      ]}
    />
  );
}

/** A pipeline graph drawn in the browser, the same drawing as lake's pipeline::render_svg: one
 * column per stage, data flow solid, control dashed. Used for a Project Agent proposal's "After",
 * which no capability renders. Steps are not links here. */
export function PipelineSvg({ graph }: { graph: Graph }) {
  const { columns, position, width, height } = layoutGraph(graph);
  const { boxWidth, boxHeight, columnWidth, left } = LAYOUT;
  return (
    <svg className="pipeline-graph" role="img" viewBox={`0 0 ${width} ${height}`} data-width={width} data-height={height} xmlns="http://www.w3.org/2000/svg">
      <title>{`${graph.name} — ${graph.steps.length} steps, ${graph.edges.length} edges`}</title>
      <defs>
        <marker id="arrow-after" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
          <path d="M 0 0 L 10 5 L 0 10 z" fill="currentColor" />
        </marker>
      </defs>
      {columns.map((column, index) => (
        <text key={column} className="lane" x={left + index * columnWidth} y={20}>
          {column}
        </text>
      ))}
      {graph.edges.map((edge, index) => {
        const from = position.get(edge.from);
        const to = position.get(edge.to);
        if (!from || !to) return null;
        const same = from.x === to.x;
        const x1 = same ? from.x + boxWidth / 2 : from.x + boxWidth;
        const y1 = same ? from.y + boxHeight : from.y + boxHeight / 2;
        const x2 = same ? to.x + boxWidth / 2 : to.x;
        const y2 = same ? to.y : to.y + boxHeight / 2;
        return (
          <line key={index} className={`edge ${edge.kind === "control" ? "edge-control" : "edge-data"}`} x1={x1} y1={y1} x2={x2} y2={y2} markerEnd="url(#arrow-after)">
            <title>{`${edge.from} → ${edge.to} (${edge.kind})`}</title>
          </line>
        );
      })}
      {graph.steps.map((step) => {
        const at = position.get(step.id) ?? { x: 0, y: 0 };
        const kind = (["connect", "clean", "model", "index", "serve", "act"] as string[]).includes(step.kind) ? step.kind : "other";
        const name = [...step.name].length > 22 ? `${[...step.name].slice(0, 21).join("")}…` : step.name;
        return (
          <g key={step.id} className={`step step-${kind}`}>
            <rect x={at.x} y={at.y} width={boxWidth} height={boxHeight} rx={8} />
            <text x={at.x + 12} y={at.y + 24} className="step-name">
              {name}
            </text>
            <text x={at.x + 12} y={at.y + 43} className="step-meta">
              {step.id}
            </text>
            <title>{`${step.name} (${step.kind})`}</title>
          </g>
        );
      })}
    </svg>
  );
}

/** The Design breadcrumb (lake's workspace_trail): Design › the list page › the entities. */
export function useDesignTrail(list: { label: string; path: string }, entities: (Crumb | null)[], title: string) {
  const ws = useWsPath();
  const trail: Crumb[] = [{ label: "Design", to: ws("/design/projects") }, { label: list.label, to: ws(list.path) }];
  for (const e of entities) if (e) trail.push(e);
  useTrail(trail, title);
}

export { entityCrumb };

/** One form's submission: busy while it runs, the API's error (or a client-side refusal) after. */
export function useSubmit() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const run = useCallback(async (work: () => Promise<unknown>) => {
    setBusy(true);
    setError(null);
    try {
      await work();
      return true;
    } catch (e) {
      setError(e);
      return false;
    } finally {
      setBusy(false);
    }
  }, []);
  return { busy, error, setError, run };
}

/** A refusal the page makes itself, worded as the form handler worded it. */
export class Refusal extends Error {}

/** An action's error in lake's words, above a form's button: the kit's `Form error=` titles every
 * error "Couldn't load this", which reads wrong for a refused write. */
export function FormError({ error }: { error: unknown }) {
  if (!error) return null;
  return (
    <p className="error" role="alert">
      {(error as Error).message}
    </p>
  );
}
