/** The pieces several Design pages share, in lake's markup: the Runs panel, status badges, the
 * gate checks, a JSON disclosure, Markdown, the pipeline graph drawn in the browser, and the
 * workspace breadcrumb. */
import { type ReactNode, useCallback, useState } from "react";
import { Link } from "react-router-dom";
import { Badge } from "@/lib/ui";
import { useTrail, entityCrumb, type Crumb } from "@/lib/trail";
import { useWsPath, qs } from "./api";
import { LAYOUT, layoutGraph, runIsActive, runStatusTone, runsBadge, text, verdictTone, whenUtc, type Graph, type J } from "./logic";

/** A panel with its Title Case heading, as lake's `<section class="panel"><h2>`. */
export function Panel({ title, children, id, className, role }: { title?: ReactNode; children?: ReactNode; id?: string; className?: string; role?: string }) {
  return (
    <section className={className ? `panel ${className}` : "panel"} id={id} role={role}>
      {title && <h2>{title}</h2>}
      {children}
    </section>
  );
}

export { Markdown } from "@/lib/markdown";

/** A run, check or monitor status as lake's badge (run_status_badge). */
export function RunBadge({ status }: { status: string }) {
  return <Badge tone={runStatusTone(status)}>{status || "unknown"}</Badge>;
}

/** Whether a methodology or solution runs here (runs_badge_of). */
export function RunsBadge({ entry }: { entry: J }) {
  const b = runsBadge(entry);
  return <Badge tone={b.tone}>{b.label}</Badge>;
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
    <details>
      <summary>View JSON</summary>
      <pre>{JSON.stringify(value, null, 2)}</pre>
    </details>
  );
}

/** The Runs panel of a project or solution (solution_runs_content). The page polls every 3 seconds
 * while a run is active; this says so. */
export function RunsPanel({ runs }: { runs: J[] }) {
  const ws = useWsPath();
  if (!runs.length) {
    return (
      <Panel title="Runs">
        <p>No runs yet.</p>
      </Panel>
    );
  }
  const active = runs.some(runIsActive);
  return (
    <Panel title="Runs">
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Run</th>
              <th>Status</th>
              <th>Stage Reached</th>
              <th>Started</th>
              <th>Error</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {runs.map((run, index) => {
              const id = text(run, "id");
              return (
                <tr key={id || index}>
                  <td>
                    <Identifier value={id} />
                  </td>
                  <td>
                    <RunBadge status={text(run, "status")} />
                  </td>
                  <td>{text(run, "stage") || "—"}</td>
                  <td>{whenUtc(text(run, "created"))}</td>
                  <td>{text(run, "error") ? <span className="error">{text(run, "error")}</span> : "—"}</td>
                  <td>
                    <Link to={ws(`/train/run?${qs({ id })}`)}>Open full run</Link>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      {active && <p className="muted">This page checks again every 3 seconds while a run is active.</p>}
    </Panel>
  );
}

/** Gate checks, from gate or from deploy's gate (gate_panel). */
export function GatePanel({ gate, heading }: { gate: J; heading: string }) {
  const verdict = text(gate, "verdict");
  const blocked = gate?.block_deploy === true;
  const checks: J[] = Array.isArray(gate?.checks) ? gate.checks : [];
  return (
    <section className={blocked ? "panel notice status-danger" : "panel notice"} role="status">
      <h2>{heading}</h2>
      <p>
        <Badge tone={verdictTone(verdict)}>{verdict || (blocked ? "fail" : "pass")}</Badge>{" "}
        {blocked ? "It cannot run or deploy until the failing checks pass." : "It may run and deploy."}
      </p>
      {checks.length > 0 && <CheckTable checks={checks} />}
    </section>
  );
}

export function CheckTable({ checks, runTone }: { checks: J[]; runTone?: boolean }) {
  return (
    <div className="table-wrap">
      <table className="gate-checks">
        <thead>
          <tr>
            <th>Check</th>
            <th>Status</th>
            <th>Why</th>
          </tr>
        </thead>
        <tbody>
          {checks.map((check, index) => {
            const status = text(check, "status");
            return (
              <tr key={index}>
                <td>{text(check, "name")}</td>
                <td>{runTone ? <RunBadge status={status} /> : <Badge tone={verdictTone(status)}>{status}</Badge>}</td>
                <td>{text(check, "reason")}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
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
