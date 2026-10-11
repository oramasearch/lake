/** Search (`/search?q=&in=repositories,code,datasets,drives,…`): one query across the places a person
 * chooses with checkboxes. Each place is searched through the capability its own page reads, at
 * once and side by side; results come back grouped by place, each group with its count, and a
 * place that fails or is unavailable says so without hiding the others. The choice of places is
 * the URL (and this browser remembers it).
 *
 * The places, their reads and the hits live in `@/lib/federatedPlaces`, which the top bar's search
 * uses too (lake#633). Workspace places (datasets, transcripts, projects, agents, models, drives)
 * are searched in every workspace the person has, each hit naming its workspace when there are
 * several.
 *
 * Code search keeps its options (branch or tag, exact change, complete history, an artifact) under
 * "Code options". The old `?query=` address still opens with code search. */
import { useEffect, useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { type UseQueryResult } from "@tanstack/react-query";
import { Search as SearchIcon } from "lucide-react";
import { Chip, Empty, ErrorBlock, Page, Skeleton, cx } from "@/lib/ui";
import { Highlight } from "@/components/Highlight";
import { HitLink, PLACES, unavailable, useFederatedSearch, useSearchScopes, type CodeOptions, type Place, type PlaceAnswer } from "@/lib/federatedPlaces";

const STORE = "lake-search-places";

const ALL: string[] = PLACES.map((p) => p.id);

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
  const scopes = useSearchScopes();
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
  const results = useFederatedSearch(q, chosen, scopes, options);
  const total = results.reduce((n, r) => n + (r.data?.hits.length ?? 0), 0);

  return (
    <Page eyebrow="Data" title="Search" sub="One search across your repositories, code, datasets, transcripts, projects, agents, models, drives and the docs. Choose where to look.">
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
            const count = on && q && r?.data ? r.data.hits.length : undefined;
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
            {scopes.length > 1 ? (
              <>Datasets, transcripts, projects, agents, models and drives are searched in all {scopes.length} of your workspaces.</>
            ) : scopes.length === 1 ? (
              <>
                Datasets, transcripts, projects, agents, models and drives are searched in the workspace <strong>{scopes[0].name}</strong>.
              </>
            ) : (
              <>Create a workspace to search its datasets, transcripts, projects, agents, models and drives.</>
            )}
          </p>
        )}
      </fieldset>

      {places.includes("code") && <CodeOptionsBox options={options} onApply={(extra) => go({ extra })} />}

      {!q ? (
        <Empty icon={SearchIcon} title="Search across Lake">
          Type what you are looking for and choose where to look. You can search several places at once.
        </Empty>
      ) : (
        <>
          <p className="label mb-3">
            {results.some((r) => r.isLoading) ? "Searching…" : `${total} result${total === 1 ? "" : "s"} for “${q}”`}
          </p>
          <div className="space-y-4">
            {chosen.map((place, i) => (
              <PlaceResults key={place.id} place={place} text={q} q={results[i]} needsWorkspace={place.workspace && !scopes.length} />
            ))}
          </div>
        </>
      )}
    </Page>
  );
}

function PlaceResults({ place, text, q, needsWorkspace }: { place: Place; text: string; q: UseQueryResult<PlaceAnswer>; needsWorkspace: boolean }) {
  const hits = q.data?.hits ?? [];
  const failed = q.data?.failed ?? [];
  return (
    <section className="overflow-hidden rounded-[6px] border border-hairline bg-surface">
      <header className="flex items-center gap-2 border-b border-hairline bg-raised px-4 py-2">
        <place.icon size={14} className="text-ink-3" aria-hidden />
        <h2 className="!m-0 !text-[10.5px] after:!hidden">{place.label}</h2>
        <span className="ml-auto font-mono text-[11px] text-ink-3">{q.isLoading ? "…" : q.data ? hits.length : ""}</span>
      </header>
      <div>
        {needsWorkspace ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">Create a workspace to search {place.label.toLowerCase()}.</p>
        ) : q.isLoading ? (
          <div className="px-4">
            <Skeleton lines={2} />
          </div>
        ) : unavailable(q.error) ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">Searching {place.label.toLowerCase()} is not available on this Lake yet.</p>
        ) : q.isError ? (
          <div className="p-3">
            <ErrorBlock error={q.error} retry={() => void q.refetch()} what={place.label.toLowerCase()} />
          </div>
        ) : !hits.length ? (
          <p className="muted m-0 px-4 py-3 text-[13px]">{failed.length ? "No matches in the workspaces that answered." : "No matches."}</p>
        ) : (
          <ul className="m-0 list-none p-0">
            {hits.slice(0, 50).map((hit) => (
              <li key={hit.key} className="border-b border-hairline px-4 py-2.5 last:border-b-0 hover:bg-raised/60">
                <div className="flex min-w-0 items-center gap-2 text-[13.5px] font-medium">
                  {hit.icon ? <HitIcon hit={hit} /> : null}
                  <span className="min-w-0 flex-1">
                    <HitLink hit={hit} text={text} />
                  </span>
                  {hit.workspace && <span className="shrink-0 text-[11.5px] font-normal text-ink-3">{hit.workspace}</span>}
                </div>
                {hit.detail && (
                  <div className="mt-0.5 line-clamp-2 text-[12.5px] text-ink-2">
                    {hit.detailCode ? <code className="whitespace-pre-wrap">{hit.detail}</code> : <Highlight text={hit.detail} q={text} />}
                  </div>
                )}
              </li>
            ))}
            {hits.length > 50 && <li className="px-4 py-2 text-[12.5px] text-ink-3">{hits.length - 50} more; narrow the search to see them.</li>}
          </ul>
        )}
        {failed.length > 0 && (
          <p className="muted m-0 border-t border-hairline px-4 py-2 text-[12.5px]">
            Could not search {failed.map((f) => f.workspace).join(", ")}: {failed[0].message}
          </p>
        )}
      </div>
    </section>
  );
}

function HitIcon({ hit }: { hit: { icon?: unknown; iconClass?: string } }) {
  const Icon = hit.icon as Place["icon"];
  return <Icon size={14} aria-hidden className={cx("shrink-0", hit.iconClass)} />;
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
