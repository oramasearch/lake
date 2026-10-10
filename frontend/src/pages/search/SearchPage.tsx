/** Search (`/search?q=&in=repositories,code,datasets,…`): one query across the places a person
 * chooses with checkboxes. Each place is searched through the capability its own page reads, at
 * once and side by side; results come back grouped by place, each group with its count, and a
 * place that fails or is unavailable says so without hiding the others. The choice of places is
 * the URL (and this browser remembers it).
 *
 * Code search keeps its options (branch or tag, exact change, complete history, an artifact) under
 * "Code options". The old `?query=` address still opens with code search. */
import { type ReactNode, useEffect, useMemo, useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQueries, type UseQueryResult } from "@tanstack/react-query";
import { BookMarked, BookOpen, Bot, Boxes, Code2, Database, FileAudio, FolderKanban, type LucideIcon, Search as SearchIcon } from "lucide-react";
import { ApiError, postRead, read, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Chip, Empty, ErrorBlock, Page, Skeleton, cx } from "@/lib/ui";

type Hit = { key: string; title: ReactNode; detail?: ReactNode; to?: string; href?: string };
type Place = {
  id: string;
  label: string;
  icon: LucideIcon;
  /** Needs a workspace (the session's current one). */
  workspace?: boolean;
  run: (q: string, scope: Scope, options: CodeOptions, signal: AbortSignal) => Promise<Hit[]>;
};
type Scope = { customerId: string; workspaceId: string };
type CodeOptions = { ref: string; change: string; history: boolean; repository: string };

const STORE = "lake-search-places";

function matches(q: string, ...fields: unknown[]): boolean {
  const needle = q.toLowerCase();
  return fields.some((f) => typeof f === "string" && f.toLowerCase().includes(needle));
}

function wsPath(scope: Scope, path: string): string {
  return `/w/${encodeURIComponent(scope.workspaceId)}${path}`;
}

function engine<T>(scope: Scope, capability: string, input: Row, signal: AbortSignal) {
  return postRead<T>("workspace_query", { customer_id: scope.customerId, workspace_id: scope.workspaceId, capability, input }, { workspace: null, signal });
}

/** Every place Search can look, in the order they are shown. */
const PLACES: Place[] = [
  {
    id: "repositories",
    label: "Repositories",
    icon: BookMarked,
    run: async (q, _scope, _o, signal) => {
      const value = await read<{ items?: Row[] }>("list_repositories", { limit: 500 }, { workspace: null, signal });
      return (value.items ?? [])
        .filter((r) => matches(q, r.name, r.description, r.customer_name, r.owner))
        .map((r) => ({ key: r.id, title: `${r.customer_name ?? r.owner ?? ""}${r.customer_name || r.owner ? "/" : ""}${r.name}`, detail: r.description, to: `/repository?id=${encodeURIComponent(r.id)}` }));
    },
  },
  {
    id: "code",
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
        title: (
          <>
            <span className="text-ink-3">{`${h.customer}/${h.repository}`}</span> <code>{h.path}</code>
            {h.line ? <span className="text-ink-3">:{h.line}</span> : null}
          </>
        ),
        detail: h.snippet ? <code className="whitespace-pre-wrap">{h.snippet}</code> : undefined,
        // A hit names its repository by id when the index holds one; it opens the repository.
        to: /^[0-9a-f-]{36}$/.test(String(h.repository)) ? `/repository?id=${encodeURIComponent(h.repository)}` : undefined,
      }));
    },
  },
  {
    id: "datasets",
    label: "Datasets",
    icon: Database,
    workspace: true,
    run: async (q, scope, _o, signal) => {
      const value = await read<{ rows?: Row[] }>(
        "workspace_sql",
        { customer_id: scope.customerId, workspace_id: scope.workspaceId, statement: "SELECT database, dataset FROM system.datasets" },
        { workspace: null, signal },
      );
      return (value.rows ?? [])
        .filter((r) => matches(q, r.dataset, r.database))
        .map((r) => ({ key: `${r.database}.${r.dataset}`, title: r.dataset, detail: r.database, to: wsPath(scope, `/train/dataset?name=${encodeURIComponent(r.dataset)}`) }));
    },
  },
  {
    id: "transcripts",
    label: "Transcripts",
    icon: FileAudio,
    workspace: true,
    run: async (q, scope, _o, signal) => {
      const value = await read<{ transcripts?: Row[] }>("list_transcripts", { customer_id: scope.customerId, workspace_id: scope.workspaceId, q, limit: 50 }, { workspace: null, signal });
      return (value.transcripts ?? []).map((t) => ({ key: t.id, title: t.title, detail: t.excerpt, to: wsPath(scope, `/transcript?id=${encodeURIComponent(t.id)}`) }));
    },
  },
  {
    id: "projects",
    label: "Projects",
    icon: FolderKanban,
    workspace: true,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ projects?: Row[] }>(scope, "list_projects", {}, signal);
      return (value.projects ?? [])
        .filter((p) => matches(q, p.name, p.description, p.decision))
        .map((p) => ({ key: p.name, title: p.name, detail: p.description, to: wsPath(scope, `/design/project?name=${encodeURIComponent(p.name)}`) }));
    },
  },
  {
    id: "agents",
    label: "Agents",
    icon: Bot,
    workspace: true,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ agents?: Row[] }>(scope, "list_agents", {}, signal);
      return (value.agents ?? [])
        .filter((a) => a.kind !== "orama" && matches(q, a.name, a.id, a.purpose, a.description))
        .map((a) => ({ key: a.id, title: a.name ?? a.id, detail: a.purpose ?? a.description, to: wsPath(scope, `/train/agent?id=${encodeURIComponent(a.id)}`) }));
    },
  },
  {
    id: "models",
    label: "Models",
    icon: Boxes,
    workspace: true,
    run: async (q, scope, _o, signal) => {
      const value = await engine<{ models?: Row[] }>(scope, "list_models", {}, signal);
      return (value.models ?? [])
        .filter((m) => matches(q, m.name, m.id, m.methodology, m.description))
        .map((m) => ({ key: m.id ?? m.name, title: m.name ?? m.id, detail: m.methodology ?? m.description, to: wsPath(scope, "/ship/models") }));
    },
  },
  {
    id: "docs",
    label: "Docs",
    icon: BookOpen,
    run: async (q, _scope, _o, signal) => {
      const list = await read<{ pages?: Row[] }>("list_docs", undefined, { workspace: null, signal });
      const pages = await Promise.all(
        (list.pages ?? []).map((p) => read<Row>("get_doc", { page: p.slug }, { workspace: null, signal }).catch((): Row => ({ ...p, markdown: "" }))),
      );
      return pages
        .filter((p) => matches(q, p.title, p.markdown))
        .map((p) => {
          const text = String(p.markdown ?? "");
          const at = text.toLowerCase().indexOf(q.toLowerCase());
          const excerpt = at >= 0 ? `…${text.slice(Math.max(0, at - 60), at + 100).replace(/\s+/g, " ")}…` : undefined;
          return { key: p.slug, title: p.title, detail: excerpt, to: `/docs/${encodeURIComponent(p.slug)}` };
        });
    },
  },
];

const ALL = PLACES.map((p) => p.id);

function recalled(): string[] {
  try {
    const stored = JSON.parse(localStorage.getItem(STORE) ?? "null");
    if (Array.isArray(stored) && stored.every((s) => typeof s === "string")) return stored.filter((s) => ALL.includes(s));
  } catch {
    /* storage blocked */
  }
  return ALL;
}

export default function SearchPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const { workspaceId, workspace, space, viewer } = useSession();
  // The old code-search address (`?query=`) keeps working.
  const legacy = params.get("query");
  const q = (params.get("q") ?? legacy ?? "").trim();
  const inParam = params.get("in");
  const places = useMemo(() => {
    if (inParam) return inParam.split(",").filter((p) => ALL.includes(p));
    return legacy ? ["code"] : recalled();
  }, [inParam, legacy]);
  const options: CodeOptions = {
    ref: params.get("ref") ?? "",
    change: params.get("change") ?? "",
    history: params.get("history") === "1",
    repository: params.get("repository") ?? "",
  };
  const scope: Scope = { customerId: workspace?.customer_id ?? space?.id ?? viewer.customer_id ?? "", workspaceId: workspaceId ?? "" };
  const [draft, setDraft] = useState(q);
  useEffect(() => {
    setDraft(q);
  }, [q]);

  const go = (next: { q?: string; places?: string[]; extra?: Record<string, string> }) => {
    const p = new URLSearchParams();
    const text = next.q ?? draft.trim();
    if (text) p.set("q", text);
    const chosen = next.places ?? places;
    if (chosen.length !== ALL.length) p.set("in", chosen.join(","));
    for (const [k, v] of Object.entries({ ...Object.fromEntries(["ref", "change", "repository", "history"].map((k) => [k, params.get(k) ?? ""])), ...next.extra })) {
      if (v) p.set(k, v);
    }
    try {
      localStorage.setItem(STORE, JSON.stringify(chosen));
    } catch {
      /* storage blocked */
    }
    navigate(`/search${p.toString() ? `?${p}` : ""}`);
  };
  const toggle = (id: string) => {
    const next = places.includes(id) ? places.filter((p) => p !== id) : ALL.filter((p) => p === id || places.includes(p));
    go({ places: next.length ? next : [id] });
  };

  const chosen = PLACES.filter((p) => places.includes(p.id));
  const results = useQueries({
    queries: chosen.map((place) => ({
      queryKey: ["search-place", place.id, q, place.workspace ? scope : null, place.id === "code" ? options : null],
      queryFn: ({ signal }: { signal: AbortSignal }) => place.run(q, scope, options, signal),
      enabled: !!q && (!place.workspace || (!!scope.workspaceId && !!scope.customerId)),
      staleTime: 30_000,
      retry: false,
    })),
  });
  const total = results.reduce((n, r) => n + (r.data?.length ?? 0), 0);

  return (
    <Page eyebrow="Data" title="Search" sub="One search across your repositories, code, datasets, transcripts, projects, agents, models and the docs. Choose where to look.">
      <form
        className="mb-3 flex flex-wrap items-center gap-2"
        onSubmit={(e) => {
          e.preventDefault();
          go({ q: draft.trim() });
        }}
      >
        <div className="relative min-w-0 flex-1 basis-[18rem]">
          <SearchIcon size={15} className="pointer-events-none absolute top-1/2 left-3 -translate-y-1/2 text-ink-3" aria-hidden />
          <input
            className="search-input w-full !pl-9"
            type="search"
            aria-label="Search"
            autoFocus={!q}
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            placeholder="Search everything you chose below…"
          />
        </div>
        <button type="submit" className="button button-primary">
          Search
        </button>
      </form>

      <fieldset className="m-0 mb-4 border-0 p-0">
        <legend className="label mb-2">Search in</legend>
        <div className="flex flex-wrap items-center gap-1.5">
          {PLACES.map((place) => {
            const on = places.includes(place.id);
            const r = results[chosen.findIndex((c) => c.id === place.id)];
            const count = on && q && r?.data ? r.data.length : undefined;
            return (
              <label
                key={place.id}
                className={cx(
                  "search-place inline-flex cursor-pointer items-center gap-1.5 rounded-full border px-3 py-1 text-[12.5px] transition-colors select-none",
                  on ? "border-action bg-action-soft text-action" : "border-rule bg-surface text-ink-2 hover:border-action",
                )}
              >
                <input type="checkbox" className="sr-only" checked={on} onChange={() => toggle(place.id)} />
                <span aria-hidden className={cx("inline-flex h-3.5 w-3.5 items-center justify-center rounded-[3px] border text-[10px]", on ? "border-action bg-action text-on-action" : "border-rule")}>
                  {on ? "✓" : ""}
                </span>
                <place.icon size={13} aria-hidden />
                {place.label}
                {count !== undefined && <span className="font-mono text-[10.5px] opacity-80">{count}</span>}
              </label>
            );
          })}
          <Chip onClick={() => go({ places: places.length === ALL.length ? ["code"] : ALL })}>{places.length === ALL.length ? "Code only" : "Everything"}</Chip>
        </div>
        {chosen.some((p) => p.workspace) && (
          <p className="muted mt-2 mb-0 text-[12.5px]">
            {scope.workspaceId ? (
              <>
                Datasets, transcripts, projects, agents and models are searched in the workspace <strong>{workspace?.name ?? "chosen above"}</strong>.
              </>
            ) : (
              <>Choose a workspace in the top bar to search its datasets, transcripts, projects, agents and models.</>
            )}
          </p>
        )}
      </fieldset>

      {places.includes("code") && <CodeOptionsBox options={options} onApply={(extra) => go({ extra })} />}

      {!q ? (
        <Empty icon={SearchIcon} title="Search across lake">
          Type what you are looking for and choose where to look. You can search several places at once.
        </Empty>
      ) : (
        <>
          <p className="label mb-3">
            {results.some((r) => r.isLoading) ? "Searching…" : `${total} result${total === 1 ? "" : "s"} for “${q}”`}
          </p>
          <div className="space-y-4">
            {chosen.map((place, i) => (
              <PlaceResults key={place.id} place={place} q={results[i]} needsWorkspace={!!place.workspace && !scope.workspaceId} />
            ))}
          </div>
        </>
      )}
    </Page>
  );
}

function PlaceResults({ place, q, needsWorkspace }: { place: Place; q: UseQueryResult<Hit[]>; needsWorkspace: boolean }) {
  const hits = q.data ?? [];
  const unavailable = q.error instanceof ApiError && (q.error.code === "unknown_capability" || q.error.code === "not_supported" || q.error.status === 404);
  return (
    <section className="overflow-hidden rounded-[6px] border border-hairline bg-surface">
      <header className="flex items-center gap-2 border-b border-hairline bg-raised px-4 py-2">
        <place.icon size={14} className="text-ink-3" aria-hidden />
        <h2 className="!m-0 !text-[10.5px] after:!hidden">{place.label}</h2>
        <span className="ml-auto font-mono text-[11px] text-ink-3">{q.isLoading ? "…" : q.data ? hits.length : ""}</span>
      </header>
      <div>
        {needsWorkspace ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">Choose a workspace to search {place.label.toLowerCase()}.</p>
        ) : q.isLoading ? (
          <div className="px-4">
            <Skeleton lines={2} />
          </div>
        ) : unavailable ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">Searching {place.label.toLowerCase()} is not available on this lake yet.</p>
        ) : q.isError ? (
          <div className="p-3">
            <ErrorBlock error={q.error} retry={() => void q.refetch()} what={place.label.toLowerCase()} />
          </div>
        ) : !hits.length ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">No matches.</p>
        ) : (
          <ul className="m-0 list-none p-0">
            {hits.slice(0, 50).map((hit) => (
              <li key={hit.key} className="border-b border-hairline px-4 py-2.5 last:border-b-0 hover:bg-raised/60">
                <div className="text-[13.5px] font-medium">{hit.to ? <Link to={hit.to}>{hit.title}</Link> : hit.title}</div>
                {hit.detail && <div className="mt-0.5 line-clamp-2 text-[12.5px] text-ink-2">{hit.detail}</div>}
              </li>
            ))}
            {hits.length > 50 && <li className="px-4 py-2 text-[12.5px] text-ink-3">{hits.length - 50} more; narrow the search to see them.</li>}
          </ul>
        )}
      </div>
    </section>
  );
}

/** Code search's own options: a repository, a branch or tag, an exact change, complete history. */
function CodeOptionsBox({ options, onApply }: { options: CodeOptions; onApply: (extra: Record<string, string>) => void }) {
  const [open, setOpen] = useState(!!(options.ref || options.change || options.history || options.repository));
  const [draft, setDraft] = useState(options);
  return (
    <details className="mb-4" open={open} onToggle={(e) => setOpen((e.target as HTMLDetailsElement).open)}>
      <summary className="cursor-pointer text-[12.5px] text-ink-2">Code options</summary>
      <form
        className="filters mt-2"
        onSubmit={(e) => {
          e.preventDefault();
          onApply({ repository: draft.repository, ref: draft.ref, change: draft.change, history: draft.history ? "1" : "" });
        }}
      >
        <label>
          Repository
          <input value={draft.repository} onChange={(e) => setDraft({ ...draft, repository: e.target.value })} />
        </label>
        <label>
          Branch or tag
          <input value={draft.ref} onChange={(e) => setDraft({ ...draft, ref: e.target.value })} />
        </label>
        <label>
          Exact change
          <input value={draft.change} onChange={(e) => setDraft({ ...draft, change: e.target.value })} />
        </label>
        <label>
          <input type="checkbox" checked={draft.history} onChange={(e) => setDraft({ ...draft, history: e.target.checked })} /> Scan complete history
        </label>
        <button type="submit">Apply</button>
      </form>
      <p className="muted mt-1 text-[12px]">
        Ranked text, quoted substring or /regex/. A tag uses its snapshot; a change or complete history scans. Add <code>in:artifact:&lt;name&gt;@&lt;version&gt;</code> to search a
        published artifact.
      </p>
    </details>
  );
}
