/** Federated search's places (lake#633): each place's read, the hook that runs them side by side,
 * and how a hit is drawn and opened. Data → Search (`pages/search/SearchPage.tsx`) and the top bar's
 * search (`components/CommandPalette.tsx`) both use it, so they find the same things. The plain
 * logic (ranking, merging workspaces, the excerpt, the top bar's rows) is `federated.ts`.
 *
 * Each place is searched through the capability its own page reads. A workspace place is searched in
 * every workspace the person has, at once; one that fails is named under the others' hits. Queries
 * share one cache key, so "See all results" opens with the answers the top bar already has, and a
 * stale query is cancelled through its abort signal when the text changes. */
import { useEffect, useMemo, useState } from "react";
import { Link, type NavigateFunction } from "react-router-dom";
import { useQueries, type UseQueryResult } from "@tanstack/react-query";
import { BookMarked, BookOpen, Bot, Boxes, Code2, Database, FileAudio, FolderKanban, HardDrive, Lightbulb, type LucideIcon } from "lucide-react";
import { ApiError, postRead, read, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Highlight } from "@/components/Highlight";
import { CatalogueGlyph, type CatalogueKind } from "@/components/CatalogueIcon";
import { methodologyIcon, solutionIcon, type IconChoice } from "@/lib/catalogueIcons";
import { matchIcon } from "@/pages/drives/MatchLink";
import { matchLink, matchWhere, type SearchDrivesResult } from "@/pages/drives/search";
import { PLACE_IDS, WORKSPACE_PLACES, matches, mergeWorkspaces, wsPath, type CodeOptions, type Hit, type PlaceAnswer, type PlaceId, type Scope } from "@/lib/federated";

export type { CodeOptions, Hit, PlaceAnswer, PlaceId, Scope };

export type Place = {
  id: PlaceId;
  label: string;
  icon: LucideIcon;
  /** Searched in each of the person's workspaces. */
  workspace: boolean;
  run: (q: string, scope: Scope, options: CodeOptions, signal: AbortSignal) => Promise<Hit[]>;
};

export const NO_CODE_OPTIONS: CodeOptions = { ref: "", change: "", history: false, repository: "" };

function engine<T>(scope: Scope, capability: string, input: Row, signal: AbortSignal) {
  return postRead<T>("workspace_query", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input }, { workspace: null, signal });
}

/** A hit's icon that is a solution's or methodology's catalogue icon (lake#639), drawn bare a little
 * larger than the size asked, so its detail reads beside the hit. */
function catalogueHitIcon(choice: IconChoice, kind: CatalogueKind) {
  return function CatalogueHitIcon({ size = 14, className }: { size?: number; className?: string }) {
    return <CatalogueGlyph choice={choice} kind={kind} size={size + 6} tile={false} className={className} />;
  };
}

const RUNS: Record<PlaceId, { label: string; icon: LucideIcon; run: Place["run"] }> = {
  repositories: {
    label: "Repositories",
    icon: BookMarked,
    run: async (q, _scope, _o, signal) => {
      const value = await read<{ items?: Row[] }>("list_repositories", { limit: 500 }, { workspace: null, signal });
      return (value.items ?? [])
        .filter((r) => matches(q, r.name, r.description, r.customer_name, r.owner))
        .map((r) => ({
          key: r.id,
          title: String(r.name),
          lead: r.customer_name || r.owner ? `${r.customer_name ?? r.owner}/` : undefined,
          detail: r.description || undefined,
          to: `/repository?id=${encodeURIComponent(r.id)}`,
        }));
    },
  },
  code: {
    label: "Code",
    icon: Code2,
    run: async (q, _scope, o, signal) => {
      const value = await read<{ items?: Row[] }>(
        "search",
        { query: q, limit: 100, repository: o.repository, ref: o.ref, change: o.change, history: o.history ? true : undefined },
        { workspace: null, signal },
      );
      return (value.items ?? []).map((h, i) => ({
        key: `${h.repository}:${h.path}:${h.line}:${i}`,
        title: String(h.path),
        lead: `${h.customer}/${h.repository} `,
        trail: h.line ? `:${h.line}` : undefined,
        code: true,
        detail: h.snippet || undefined,
        detailCode: true,
        // A hit names its repository by id when the index holds one; it opens the repository.
        to: /^[0-9a-f-]{36}$/.test(String(h.repository)) ? `/repository?id=${encodeURIComponent(h.repository)}` : undefined,
      }));
    },
  },
  datasets: {
    label: "Datasets",
    icon: Database,
    run: async (q, scope, _o, signal) => {
      const value = await read<{ rows?: Row[] }>(
        "workspace_sql",
        { customer_id: scope.customerId, workspace_id: scope.workspaceId, statement: "SELECT database, dataset FROM system.datasets" },
        { workspace: null, signal },
      );
      return (value.rows ?? [])
        .filter((r) => matches(q, r.dataset, r.database))
        .map((r) => ({ key: `${r.database}.${r.dataset}`, title: String(r.dataset), detail: r.database, to: wsPath(scope, `/train/dataset?name=${encodeURIComponent(r.dataset)}`) }));
    },
  },
  transcripts: {
    label: "Transcripts",
    icon: FileAudio,
    // list_transcripts keeps those whose title or text holds q, and answers the text around the
    // match as the excerpt (lake#633).
    run: async (q, scope, _o, signal) => {
      const value = await read<{ transcripts?: Row[] }>("list_transcripts", { customer_id: scope.customerId, workspace_id: scope.workspaceId, q, limit: 50 }, { workspace: null, signal });
      return (value.transcripts ?? []).map((t) => ({ key: t.id, title: String(t.title), detail: t.excerpt || undefined, to: wsPath(scope, `/transcript?id=${encodeURIComponent(t.id)}`) }));
    },
  },
  projects: {
    label: "Projects",
    icon: FolderKanban,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ projects?: Row[] }>(scope, "list_projects", {}, signal);
      return (value.projects ?? [])
        .filter((p) => matches(q, p.name, p.description, p.decision))
        .map((p) => ({ key: p.name, title: String(p.name), detail: p.description || undefined, to: wsPath(scope, `/design/project?name=${encodeURIComponent(p.name)}`) }));
    },
  },
  solutions: {
    label: "Solutions",
    icon: Lightbulb,
    // Orama's catalogue and the workspace's own (lake's list_solutions), and the methodologies the
    // workspace's engine lists (absent on an older engine), each with its catalogue icon (lake#639).
    run: async (q, scope, _o, signal) => {
      const [solutions, methodologies] = await Promise.all([
        read<{ solutions?: Row[] }>("list_solutions", { customer_id: scope.customerId, workspace_id: scope.workspaceId }, { workspace: null, signal }),
        engine<{ methodologies?: Row[] }>(scope, "list_methodologies", {}, signal).catch(() => ({ methodologies: [] as Row[] })),
      ]);
      return [
        ...(solutions.solutions ?? [])
          .filter((e) => matches(q, e.name, e.id, e.promise, e.problem, e.decision))
          .map((e) => ({
            key: `solution:${e.id}`,
            title: String(e.name ?? e.id),
            lead: e.source === "workspace" ? undefined : `${e.id} `,
            detail: e.promise || e.decision || e.problem || undefined,
            icon: catalogueHitIcon(solutionIcon(e), "solution"),
            to: wsPath(scope, `/design/solution?id=${encodeURIComponent(e.id)}`),
          })),
        ...(methodologies.methodologies ?? [])
          .filter((m) => matches(q, m.name, m.id, m.family))
          .map((m) => ({
            key: `methodology:${m.id}`,
            title: String(m.name ?? m.id),
            lead: `${m.id} `,
            detail: m.family ? `Methodology · ${m.family}` : "Methodology",
            icon: catalogueHitIcon(methodologyIcon(m), "methodology"),
            to: wsPath(scope, `/design/methodology?id=${encodeURIComponent(m.id)}`),
          })),
      ];
    },
  },
  agents: {
    label: "Agents",
    icon: Bot,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ agents?: Row[] }>(scope, "list_agents", {}, signal);
      return (value.agents ?? [])
        .filter((a) => a.kind !== "orama" && matches(q, a.name, a.id, a.purpose, a.description))
        .map((a) => ({ key: a.id, title: String(a.name ?? a.id), detail: a.purpose ?? a.description ?? undefined, to: wsPath(scope, `/train/agent?id=${encodeURIComponent(a.id)}`) }));
    },
  },
  models: {
    label: "Models",
    icon: Boxes,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ models?: Row[] }>(scope, "list_models", {}, signal);
      return (value.models ?? [])
        // A model opens its training page, which links to serving it (lake#599).
        .filter((m) => matches(q, m.name, m.id, m.kind, m.pipeline_name, m.project_name))
        .map((m) => ({
          key: m.id ?? m.name,
          title: String(m.name ?? m.id),
          detail: [m.kind, m.pipeline_name || "no pipeline", m.project_name].filter(Boolean).join(" · "),
          to: wsPath(scope, `/train/model?id=${encodeURIComponent(m.id ?? "")}`),
        }));
    },
  },
  drives: {
    label: "Drives",
    icon: HardDrive,
    // One read: search_drives matches drive, folder, file and shortcut names in every drive of the
    // workspace, a repository drive's paths at main included (lake#561).
    run: async (q, scope, _o, signal) => {
      const value = await read<SearchDrivesResult>("search_drives", { customer_id: scope.customerId, workspace_id: scope.workspaceId, q, limit: 100 }, { workspace: null, signal });
      return (value.items ?? []).map((m) => {
        const link = matchLink(m, scope);
        const { icon, className } = matchIcon(m);
        return {
          key: `${m.drive.id}:${m.kind}:${m.id ?? m.path}`,
          title: m.name,
          detail: matchWhere(m),
          icon,
          iconClass: className,
          ...("to" in link ? { to: link.workspace ? wsPath(scope, link.to) : link.to } : { href: link.href, newTab: link.newTab, download: link.download }),
        };
      });
    },
  },
  docs: {
    label: "Docs",
    icon: BookOpen,
    // One read: list_docs searches every guide's title, headings and text, and says which heading
    // to open (lake#561).
    run: async (q, _scope, _o, signal) => {
      const list = await read<{ pages?: Row[] }>("list_docs", { q }, { workspace: null, signal });
      return (list.pages ?? []).map((p) => ({
        key: `${p.slug}#${p.anchor ?? ""}`,
        title: String(p.title),
        detail: p.excerpt || undefined,
        to: `/docs/${encodeURIComponent(p.slug)}${p.anchor ? `#${p.anchor}` : ""}`,
      }));
    },
  },
};

/** Every place search can look, in the order they are shown. */
export const PLACES: Place[] = PLACE_IDS.map((id) => ({ id, ...RUNS[id], workspace: WORKSPACE_PLACES.includes(id) }));

/** Searches `place` for `q`: once, or in each of `scopes` for a workspace place. */
export async function runPlace(place: Place, q: string, scopes: Scope[], options: CodeOptions, signal: AbortSignal): Promise<PlaceAnswer> {
  if (!place.workspace) return { hits: await place.run(q, { customerId: "", workspaceId: "" }, options, signal), failed: [] };
  const settled = await Promise.allSettled(scopes.map((scope) => place.run(q, scope, options, signal)));
  return mergeWorkspaces(scopes, settled);
}

/** Every workspace the person has, as search scopes (the workspace they are in first). */
export function useSearchScopes(): Scope[] {
  const { workspaces, workspaceId } = useSession();
  return useMemo(() => {
    const all = workspaces.filter((w) => w.id && w.customer_id).map((w) => ({ customerId: w.customer_id, workspaceId: w.id, name: w.name || w.id }));
    return [...all.filter((s) => s.workspaceId === workspaceId), ...all.filter((s) => s.workspaceId !== workspaceId)];
  }, [workspaces, workspaceId]);
}

/** `value`, once it has stopped changing for `ms`. */
export function useDebounced<T>(value: T, ms: number): T {
  const [settled, setSettled] = useState(value);
  useEffect(() => {
    const timer = window.setTimeout(() => setSettled(value), ms);
    return () => window.clearTimeout(timer);
  }, [value, ms]);
  return settled;
}

/** Runs `q` in `places` side by side: one query per place, each answering on its own. */
export function useFederatedSearch(q: string, places: Place[], scopes: Scope[], options: CodeOptions = NO_CODE_OPTIONS): UseQueryResult<PlaceAnswer>[] {
  const scopeKey = scopes.map((s) => `${s.customerId}/${s.workspaceId}`).join(",");
  return useQueries({
    queries: places.map((place) => ({
      queryKey: ["search-place", place.id, q, place.workspace ? scopeKey : null, place.id === "code" ? options : null],
      queryFn: ({ signal }: { signal: AbortSignal }) => runPlace(place, q, scopes, options, signal),
      enabled: !!q && (!place.workspace || scopes.length > 0),
      staleTime: 30_000,
      retry: false,
    })),
  });
}

/** A place this lake does not offer answers unknown_capability, not_supported or 404. */
export function unavailable(error: unknown): boolean {
  return error instanceof ApiError && (error.code === "unknown_capability" || error.code === "not_supported" || error.status === 404);
}

/** A hit's title: the match marked, a code hit's repository and line muted around its path. */
export function HitTitleText({ hit, text }: { hit: Hit; text: string }) {
  const title = <Highlight text={hit.title} q={text} />;
  return (
    <>
      {hit.lead && <span className="text-ink-3">{hit.lead}</span>}
      {hit.code ? <code>{title}</code> : title}
      {hit.trail && <span className="text-ink-3">{hit.trail}</span>}
    </>
  );
}

/** A hit's title as a link to what it opens. */
export function HitLink({ hit, text }: { hit: Hit; text: string }) {
  const title = <HitTitleText hit={hit} text={text} />;
  if (hit.to) return <Link to={hit.to}>{title}</Link>;
  if (hit.href) {
    return (
      <a href={hit.href} {...(hit.newTab ? { target: "_blank", rel: "noopener noreferrer" } : {})} {...(hit.download ? { download: "" } : {})}>
        {title}
      </a>
    );
  }
  return title;
}

/** Opens what a hit names, as its link would: a page of the app, or a URL the browser opens. */
export function openHit(hit: Hit, navigate: NavigateFunction) {
  if (hit.to) {
    navigate(hit.to);
    return;
  }
  if (!hit.href) return;
  const a = document.createElement("a");
  a.href = hit.href;
  if (hit.newTab) {
    a.target = "_blank";
    a.rel = "noopener noreferrer";
  }
  if (hit.download) a.download = "";
  document.body.appendChild(a);
  a.click();
  a.remove();
}
