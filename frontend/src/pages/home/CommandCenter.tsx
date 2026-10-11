/** Home for a signed-in person: the whole platform at a glance, like rev-os's Command Center.
 *
 * A workspace multi-select (`?ws=<id>,<id>`, "All workspaces" by default; remembered in this
 * browser) chooses what the figures add up: repositories, datasets, transcripts, projects, agents,
 * runs, models, search indexes, apps and services. Beside them: what needs attention (failed runs,
 * apps that are not ready, workspaces that are not ready, services in trouble), a table with each
 * workspace's own numbers, and the person's recent activity. Every figure comes from the read its
 * own page uses, per workspace, at once; a workspace whose Zetta is not ready is counted as such
 * and its engine reads are not made. */
import { type ReactNode, useMemo } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQueries, useQuery } from "@tanstack/react-query";
import {
  AlertTriangle,
  BookMarked,
  Bot,
  Boxes,
  Database,
  FileAudio,
  FolderKanban,
  Layers,
  type LucideIcon,
  Play,
  Rocket,
  Search,
  Server,
} from "lucide-react";
import { postRead, read, type Row } from "@/lib/api";
import { pollUnlessFailed } from "@/lib/errors";
import { useSession, type WorkspaceChoice } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Button, Card, Empty, Page, Skeleton, Status, cx } from "@/lib/ui";
import { relative } from "@/lib/format";

const STORE = "lake-home-workspaces";

type Figures = {
  ready: boolean;
  phase: string;
  datasets: number | null;
  transcripts: number | null;
  projects: number | null;
  agents: number | null;
  runs: Row[] | null;
  models: number | null;
  indexes: number | null;
  apps: Row[] | null;
};

const count = (value: unknown, field: string): number | null => {
  const list = (value as Row | null)?.[field];
  return Array.isArray(list) ? list.length : null;
};

/** Every figure of one workspace, each read on its own so one failure does not hide the others. */
async function figuresOf(w: WorkspaceChoice, signal: AbortSignal): Promise<Figures> {
  const base = { customer_id: w.customer_id, workspace_id: w.id };
  const opts = { workspace: null, signal } as const;
  const status = await read<Row>("workspace_status", base, opts).catch(() => null);
  const ready = !!status?.ready;
  const phase = String(status?.phase ?? (status?.requested ? "Pending" : "Not started"));
  const engine = (capability: string) => postRead<Row>("workspace_query", { ...base, capability, input: {} }, opts).catch(() => null);
  const [sql, transcripts, projects, agents, runs, models, indexes, apps] = await Promise.all([
    ready ? read<Row>("workspace_sql", { ...base, statement: "SELECT count(*) AS n FROM system.datasets" }, opts).catch(() => null) : null,
    ready ? read<Row>("list_transcripts", { ...base, limit: 500 }, opts).catch(() => null) : null,
    ready ? engine("list_projects") : null,
    ready ? engine("list_agents") : null,
    ready ? engine("list_runs") : null,
    ready ? engine("list_models") : null,
    ready ? engine("list_indexes") : null,
    read<Row>("list_apps", base, opts).catch(() => null),
  ]);
  const n = (sql?.rows as Row[] | undefined)?.[0]?.n;
  return {
    ready,
    phase,
    datasets: n === undefined || n === null ? null : Number(n),
    transcripts: count(transcripts, "transcripts"),
    projects: count(projects, "projects"),
    agents: Array.isArray(agents?.agents) ? (agents!.agents as Row[]).filter((a) => a.kind !== "orama").length : null,
    runs: Array.isArray(runs?.runs) ? (runs!.runs as Row[]) : null,
    models: count(models, "models"),
    indexes: count(indexes, "indexes"),
    apps: Array.isArray(apps?.apps) ? (apps!.apps as Row[]) : null,
  };
}

const sum = (values: (number | null | undefined)[]): number | null => {
  const known = values.filter((v): v is number => typeof v === "number");
  return known.length ? known.reduce((a, b) => a + b, 0) : null;
};

/** "Run of <pipeline> in <project> failed", from whichever names the run carries. */
function runTitle(run: Row): string {
  const name = run.pipeline_name || run.graph;
  const project = run.pipeline_name ? run.project_name : run.project;
  if (name && project) return `Run of ${name} in ${project} failed`;
  if (name || project) return `Run of ${name || project} failed`;
  return "A run failed";
}

function runState(run: Row): string {
  return String(run.status ?? run.state ?? "").toLowerCase();
}

export default function CommandCenter() {
  useTrail([{ label: "Home" }], "Home");
  const { viewer, workspaces } = useSession();
  const [params] = useSearchParams();
  const navigate = useNavigate();

  const chosenIds = useMemo(() => {
    const fromUrl = params.get("ws");
    if (fromUrl) return fromUrl.split(",").filter((id) => workspaces.some((w) => w.id === id));
    try {
      const stored = JSON.parse(localStorage.getItem(STORE) ?? "null");
      if (Array.isArray(stored)) return stored.filter((id: unknown) => typeof id === "string" && workspaces.some((w) => w.id === id));
    } catch {
      /* storage blocked */
    }
    return [];
  }, [params, workspaces]);
  const all = chosenIds.length === 0 || chosenIds.length === workspaces.length;
  const chosen = all ? workspaces : workspaces.filter((w) => chosenIds.includes(w.id));

  const choose = (ids: string[]) => {
    const next = ids.length === workspaces.length ? [] : ids;
    try {
      localStorage.setItem(STORE, JSON.stringify(next));
    } catch {
      /* storage blocked */
    }
    navigate(next.length ? `/?ws=${next.map(encodeURIComponent).join(",")}` : "/", { replace: true });
  };
  const toggle = (id: string) => {
    const current = all ? [] : chosenIds;
    const next = current.includes(id) ? current.filter((x) => x !== id) : [...current, id];
    choose(next.length ? next : []);
  };

  const perWorkspace = useQueries({
    queries: chosen.map((w) => ({
      queryKey: ["home-figures", w.id],
      queryFn: ({ signal }: { signal: AbortSignal }) => figuresOf(w, signal),
      staleTime: 30_000,
      refetchInterval: pollUnlessFailed(60_000),
    })),
  });
  const repositories = useQuery({
    queryKey: ["home-repositories"],
    queryFn: ({ signal }) => read<{ items?: Row[] }>("list_repositories", { limit: 500 }, { workspace: null, signal }),
    staleTime: 30_000,
  });
  const spaceIds = [...new Set(chosen.map((w) => w.customer_id))];
  const services = useQueries({
    queries: spaceIds.map((id) => ({
      queryKey: ["home-services", id],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<{ services?: Row[] }>("list_services", { customer_id: id }, { workspace: null, signal }),
      staleTime: 30_000,
    })),
  });
  const activity = useQuery({
    queryKey: ["home-activity"],
    queryFn: ({ signal }) => read<{ items?: Row[] }>("my_audit", { limit: 12, order: "newest" }, { workspace: null, signal }),
    staleTime: 30_000,
  });

  const loading = perWorkspace.some((q) => q.isLoading);
  const figures = perWorkspace.map((q) => q.data);
  const chosenSet = new Set(chosen.map((w) => w.id));
  const repoRows = (repositories.data?.items ?? []).filter((r) => !r.workspace_id || chosenSet.has(r.workspace_id));
  const serviceRows = services.flatMap((q) => q.data?.services ?? []).filter((s) => !s.workspace_id || chosenSet.has(s.workspace_id));
  const runs = figures.flatMap((f) => f?.runs ?? []);
  const running = runs.filter((r) => /^(running|queued|pending|starting)$/.test(runState(r))).length;
  const failedRuns = runs.filter((r) => /^(failed|error|errored)$/.test(runState(r)));
  const apps = figures.flatMap((f) => f?.apps ?? []);
  const appsDown = apps.filter((a) => !a.ready);
  const notReady = chosen.filter((_, i) => figures[i] && !figures[i]!.ready);
  const servicesDown = serviceRows.filter((s) => /fail|error|degraded/i.test(String(s.status ?? "")));

  const stats: { label: string; value: number | null; icon: LucideIcon; to?: string; sub?: ReactNode; tone?: "bad" }[] = [
    { label: "Repositories", value: repositories.data ? repoRows.length : null, icon: BookMarked, to: "/repositories" },
    { label: "Datasets", value: sum(figures.map((f) => f?.datasets)), icon: Database, to: "/train/data" },
    { label: "Transcripts", value: sum(figures.map((f) => f?.transcripts)), icon: FileAudio, to: "/transcripts" },
    { label: "Projects", value: sum(figures.map((f) => f?.projects)), icon: FolderKanban, to: "/design/projects" },
    { label: "Agents", value: sum(figures.map((f) => f?.agents)), icon: Bot, to: "/train/agents" },
    {
      label: "Runs",
      value: runs.length || (figures.some((f) => f?.runs) ? 0 : null),
      icon: Play,
      to: "/train/runs",
      sub: runs.length ? `${running} active · ${failedRuns.length} failed` : undefined,
      tone: failedRuns.length ? "bad" : undefined,
    },
    { label: "Models", value: sum(figures.map((f) => f?.models)), icon: Boxes, to: "/ship/models" },
    { label: "Search Indexes", value: sum(figures.map((f) => f?.indexes)), icon: Search, to: "/ship/indexes" },
    { label: "Apps", value: figures.some((f) => f?.apps) ? apps.length : null, icon: Rocket, to: "/ship/apps", sub: apps.length ? `${apps.length - appsDown.length} ready` : undefined },
    { label: "Services", value: services.some((q) => q.data) ? serviceRows.length : null, icon: Server, to: "/services" },
  ];

  const attention: { key: string; tone: "bad" | "warn"; title: ReactNode; detail: ReactNode; to?: string }[] = [
    ...notReady.map((w) => {
      const f = figures[chosen.indexOf(w)]!;
      return { key: `ws-${w.id}`, tone: (f.phase === "Failed" ? "bad" : "warn") as "bad" | "warn", title: `Workspace ${w.name} is not ready`, detail: f.phase, to: `/w/${w.id}/design/projects` };
    }),
    ...failedRuns.slice(0, 6).map((r, i) => ({
      key: `run-${r.id ?? i}`,
      tone: "bad" as const,
      title: runTitle(r),
      detail: r.error ?? r.message ?? relative(r.created),
      to: r.id ? `/train/run?id=${encodeURIComponent(r.id)}` : "/train/runs",
    })),
    ...appsDown.slice(0, 6).map((a, i) => ({
      key: `app-${a.id ?? i}`,
      tone: (String(a.phase).toLowerCase() === "failed" ? "bad" : "warn") as "bad" | "warn",
      title: `App ${a.name ?? a.id} is not ready`,
      detail: a.phase ?? "Pending",
      to: a.id ? `/ship/app?app_id=${encodeURIComponent(a.id)}` : "/ship/apps",
    })),
    ...servicesDown.slice(0, 6).map((s, i) => ({ key: `svc-${s.name ?? i}`, tone: "bad" as const, title: `Service ${s.name} needs attention`, detail: s.status_reason ?? s.status, to: "/services" })),
  ];

  const name = viewer.display_name?.split(" ")[0];

  return (
    <Page
      eyebrow="Home"
      title={name ? `Welcome back, ${name}` : "Home"}
      sub="Everything in lake at a glance: what you have, what is running, and what needs you."
      actions={
        <>
          <Button to="/train/data">Load data</Button>
          <Button to="/design/projects" variant="primary">
            New project
          </Button>
        </>
      }
      wide
    >
      {workspaces.length === 0 ? (
        <Empty icon={Layers} title="Start your first workspace" action={<Button to="/workspaces" variant="primary">Workspaces</Button>}>
          A workspace holds your data, projects, agents and models. Create one to begin.
        </Empty>
      ) : (
        <>
          <fieldset className="m-0 mb-4 border-0 p-0">
            <legend className="label mb-2">Workspaces</legend>
            <div className="flex flex-wrap items-center gap-1.5">
              <WorkspaceToggle on={all} onChange={() => choose([])} label="All workspaces" count={workspaces.length} />
              {workspaces.map((w) => (
                <WorkspaceToggle key={w.id} on={!all && chosenIds.includes(w.id)} onChange={() => toggle(w.id)} label={w.name} />
              ))}
            </div>
          </fieldset>

          <div className="mb-5 grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-5">
            {stats.map((s) => (
              <Link key={s.label} to={s.to ?? "#"} className="block text-inherit no-underline">
                <div className={cx("h-full rounded-[6px] border border-hairline bg-surface px-4 py-3 transition-colors hover:border-rule", s.tone === "bad" && "border-l-[3px] border-l-bad")}>
                  <div className="flex items-center gap-2">
                    <s.icon size={13} className="text-ink-3" />
                    <div className="label m-0 truncate">{s.label}</div>
                  </div>
                  <div className="mt-1.5 font-mono text-[22px] leading-none font-semibold text-ink tabular-nums">
                    {s.value === null ? (loading ? <span className="text-ink-3">…</span> : <span className="text-ink-3">—</span>) : s.value.toLocaleString()}
                  </div>
                  {s.sub && <div className="mt-1.5 text-[12px] text-ink-3">{s.sub}</div>}
                </div>
              </Link>
            ))}
          </div>

          <div className="grid grid-cols-1 gap-4 xl:grid-cols-[minmax(0,1.55fr)_minmax(0,1fr)]">
            <div className="min-w-0 space-y-4">
              <Card label={`Needs attention${attention.length ? ` (${attention.length})` : ""}`} tone={attention.length ? "exception" : "default"} padded={false}>
                {loading && !attention.length ? (
                  <div className="px-4 pb-3">
                    <Skeleton lines={2} />
                  </div>
                ) : attention.length ? (
                  <ul className="m-0 list-none p-0">
                    {attention.map((a) => (
                      <li key={a.key} className="flex items-start gap-3 border-t border-hairline px-4 py-2.5">
                        <AlertTriangle size={14} className={cx("mt-[3px] shrink-0", a.tone === "bad" ? "text-bad" : "text-warn")} aria-hidden />
                        <div className="min-w-0 flex-1">
                          <div className="text-[13.5px] font-medium text-ink">{a.title}</div>
                          {a.detail && <div className="truncate text-[12.5px] text-ink-3">{a.detail}</div>}
                        </div>
                        {a.to && (
                          <Link to={a.to} className="shrink-0 text-[12.5px]">
                            Open →
                          </Link>
                        )}
                      </li>
                    ))}
                  </ul>
                ) : (
                  <p className="muted m-0 border-t border-hairline px-4 py-3 text-[13px]">Nothing needs you. Every workspace is ready and nothing has failed.</p>
                )}
              </Card>

              <Card label="By workspace" padded={false}>
                <div className="table-wrap !rounded-none !border-0 !border-t">
                  <table>
                    <thead>
                      <tr>
                        <th>Workspace</th>
                        <th>Status</th>
                        <th className="text-right">Datasets</th>
                        <th className="text-right">Projects</th>
                        <th className="text-right">Agents</th>
                        <th className="text-right">Runs</th>
                        <th className="text-right">Models</th>
                        <th className="text-right">Apps</th>
                      </tr>
                    </thead>
                    <tbody>
                      {chosen.map((w, i) => {
                        const f = figures[i];
                        const cell = (v: number | null | undefined) => (
                          <td className="text-right font-mono tabular-nums">{f === undefined ? "…" : v === null || v === undefined ? "—" : v}</td>
                        );
                        return (
                          <tr key={w.id}>
                            <td>
                              <Link to={`/w/${w.id}/design/projects`}>{w.name}</Link>
                            </td>
                            <td>{f ? <Status value={f.ready ? "ready" : f.phase} /> : <span className="muted">…</span>}</td>
                            {cell(f?.datasets)}
                            {cell(f?.projects)}
                            {cell(f?.agents)}
                            {cell(f?.runs?.length)}
                            {cell(f?.models)}
                            {cell(f?.apps?.length)}
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
              </Card>
            </div>

            <div className="min-w-0 space-y-4">
              <Card label="Recent activity" actions={<Link to="/activity" className="text-[12.5px]">All activity →</Link>} padded={false}>
                {activity.isLoading ? (
                  <div className="px-4 pb-3">
                    <Skeleton lines={3} />
                  </div>
                ) : activity.isError ? (
                  <p className="muted m-0 border-t border-hairline px-4 py-3 text-[13px]">Your activity could not be read.</p>
                ) : (activity.data?.items ?? []).length ? (
                  <ul className="m-0 list-none p-0">
                    {(activity.data?.items ?? []).slice(0, 12).map((e, i) => (
                      <li key={e.id ?? i} className="flex items-baseline gap-3 border-t border-hairline px-4 py-2">
                        <span className="w-16 shrink-0 font-mono text-[11px] text-ink-3">{relative(e.occurred_at ?? e.timestamp)}</span>
                        <span className="min-w-0 flex-1 truncate text-[13px] text-ink">
                          {String(e.capability || e.action || "").replace(/_/g, " ")} <span className="text-ink-3">{String(e.object ?? "").replace(/^capability\//, "")}</span>
                        </span>
                        <Status value={String(e.outcome ?? "")} />
                      </li>
                    ))}
                  </ul>
                ) : (
                  <p className="muted m-0 border-t border-hairline px-4 py-3 text-[13px]">Nothing yet.</p>
                )}
              </Card>
              <Card label="Quick Launch">
                <div className="grid grid-cols-2 gap-2">
                  <Button to="/train/data">Load data</Button>
                  <Button to="/transcripts">Add a transcript</Button>
                  <Button to="/design/opportunities">See opportunities</Button>
                  <Button to="/train/agents">Build an agent</Button>
                  <Button to="/ship/apps">Ship an app</Button>
                  <Button to="/search">Search everything</Button>
                </div>
              </Card>
            </div>
          </div>
        </>
      )}
    </Page>
  );
}

function WorkspaceToggle({ on, onChange, label, count }: { on: boolean; onChange: () => void; label: string; count?: number }) {
  return (
    <label
      className={cx(
        "inline-flex cursor-pointer items-center gap-1.5 rounded-full border px-3 py-1 text-[12.5px] transition-colors select-none",
        on ? "border-action bg-action-soft text-action" : "border-rule bg-surface text-ink-2 hover:border-action",
      )}
    >
      <input type="checkbox" className="sr-only" checked={on} onChange={onChange} />
      <span aria-hidden className={cx("inline-flex h-3.5 w-3.5 items-center justify-center rounded-[3px] border text-[10px]", on ? "border-action bg-action text-on-action" : "border-rule")}>
        {on ? "✓" : ""}
      </span>
      {label}
      {count !== undefined && <span className="font-mono text-[10.5px] opacity-80">{count}</span>}
    </label>
  );
}
