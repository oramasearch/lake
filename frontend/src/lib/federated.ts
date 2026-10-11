/** Federated search as plain logic (lake#633): what a hit is, the places in order, how each place's
 * answers from several workspaces become one, how hits rank, the excerpt around a match, the rows
 * of the top bar's search and the address of "See all results". Data → Search and the top bar both
 * use it through `federatedPlaces.tsx`. Pure, so `tests/federated.test.ts` runs it under Node. */

/** Every place search looks, in the order they are shown. `federatedPlaces.tsx` gives each its
 * read; the top bar and Data → Search show them in this order. */
export const PLACE_IDS = ["repositories", "code", "datasets", "transcripts", "projects", "agents", "models", "drives", "docs"] as const;
export type PlaceId = (typeof PLACE_IDS)[number];

/** Places whose items live in a workspace, searched in every workspace the person has. */
export const WORKSPACE_PLACES: readonly PlaceId[] = ["datasets", "transcripts", "projects", "agents", "models", "drives"];

/** One workspace a search runs in. */
export type Scope = { customerId: string; workspaceId: string; name?: string };
export type CodeOptions = { ref: string; change: string; history: boolean; repository: string };

/** One result. `title` shows the match marked. `lead` and `trail` sit muted around it (a code
 * hit's repository and line); `code` sets the title as code. `detail` is the second line: where it
 * lives, or the text around the match. `workspace` names the workspace it came from when the person
 * has more than one. `to` is a page of the app; `href` a URL the browser opens itself (a file, in a
 * new tab when `newTab`, saved when `download`). `icon` is a lucide icon (`federatedPlaces.tsx`). */
export type Hit = {
  key: string;
  title: string;
  lead?: string;
  trail?: string;
  code?: boolean;
  detail?: string;
  detailCode?: boolean;
  workspace?: string;
  to?: string;
  href?: string;
  newTab?: boolean;
  download?: boolean;
  icon?: unknown;
  iconClass?: string;
};

/** A place's answer: its hits, and the workspaces it could not search (named) while others
 * answered. */
export type PlaceAnswer = { hits: Hit[]; failed: { workspace: string; message: string }[] };

export function matches(q: string, ...fields: unknown[]): boolean {
  const needle = q.toLowerCase();
  return fields.some((f) => typeof f === "string" && f.toLowerCase().includes(needle));
}

/** A workspace page's address in `scope`'s workspace. */
export function wsPath(scope: Scope, path: string): string {
  return `/w/${encodeURIComponent(scope.workspaceId)}${path}`;
}

/** One workspace place's answers from every workspace, as one: the hits in workspace order, each
 * named by its workspace when there is more than one, and the workspaces that failed. Every
 * workspace failing is the place failing: it throws the first error. No workspace at all answers
 * nothing. */
export function mergeWorkspaces(scopes: Scope[], settled: PromiseSettledResult<Hit[]>[]): PlaceAnswer {
  const several = scopes.length > 1;
  const hits: Hit[] = [];
  const failed: PlaceAnswer["failed"] = [];
  settled.forEach((result, i) => {
    const scope = scopes[i];
    const name = scope.name || scope.workspaceId;
    if (result.status === "fulfilled") {
      for (const hit of result.value) hits.push(several ? { ...hit, key: `${scope.workspaceId}:${hit.key}`, workspace: name } : hit);
    } else {
      failed.push({ workspace: name, message: result.reason instanceof Error ? result.reason.message : String(result.reason) });
    }
  });
  if (settled.length && failed.length === settled.length) {
    const first = settled.find((r) => r.status === "rejected") as PromiseRejectedResult;
    throw first.reason;
  }
  return { hits, failed };
}

/** How well a hit matches `q`: 0 the title is the query, 1 the title starts with it, 2 a word of
 * the title does, 3 the title holds it, 4 only its detail or content does. Lower is better. */
export function matchRank(hit: Pick<Hit, "title">, q: string): number {
  const needle = q.trim().toLowerCase();
  const title = hit.title.toLowerCase();
  if (!needle) return 4;
  if (title === needle) return 0;
  if (title.startsWith(needle)) return 1;
  const at = title.indexOf(needle);
  if (at > 0 && /[^a-z0-9]/.test(title[at - 1])) return 2;
  if (at >= 0) return 3;
  return 4;
}

/** A place's hits ordered by `matchRank`, keeping the place's own order among equals (code search
 * and docs come ranked by their read). */
export function rankHits(hits: Hit[], q: string): Hit[] {
  return hits
    .map((hit, i) => ({ hit, i, rank: matchRank(hit, q) }))
    .sort((a, b) => a.rank - b.rank || a.i - b.i)
    .map((x) => x.hit);
}

/** The top bar's few hits of a place: the best `n`. */
export function topHits(hits: Hit[], q: string, n = 4): Hit[] {
  return rankHits(hits, q).slice(0, n);
}

/** About `max` characters of `text` on one line around the first match of `q` (ignoring case),
 * starting at a word, with "…" where it was cut. Without a match, the start. */
export function excerptAround(text: string, q: string, max = 120): string {
  const line = text.replace(/\s+/g, " ").trim();
  if (line.length <= max) return line;
  const needle = q.trim().toLowerCase();
  const at = needle ? line.toLowerCase().indexOf(needle) : -1;
  if (at < 0) return `${line.slice(0, max - 1).trimEnd()}…`;
  let start = Math.max(0, at - Math.floor(max / 3));
  if (start > 0) {
    const space = line.indexOf(" ", start);
    if (space >= 0 && space < at) start = space + 1;
  }
  // A leading "…" the server already put there stays the only one.
  if (start > 0 && line.slice(0, start).trim() === "…") start = 0;
  let end = Math.min(line.length, start + max);
  if (end < line.length) {
    const space = line.lastIndexOf(" ", end);
    if (space > at + needle.length) end = space;
  }
  return `${start > 0 ? "…" : ""}${line.slice(start, end).trim()}${end < line.length ? "…" : ""}`;
}

/** The second line of a hit in the top bar: its workspace (when the person has several), then where
 * it lives or the text around the match. */
export function hitWhere(hit: Hit, q: string, max = 110): string {
  const detail = hit.detail ? excerptAround(hit.detail, q, max) : "";
  return [hit.workspace, detail].filter(Boolean).join(" · ");
}

/** Data → Search with `q` and every place on. */
export function searchAllUrl(q: string): string {
  const p = new URLSearchParams();
  if (q.trim()) p.set("q", q.trim());
  p.set("in", PLACE_IDS.join(","));
  return `/search?${p}`;
}

/** A row of the top bar's search: a group's heading, an item that opens, or a muted line. */
export type PaletteRow =
  | { kind: "group"; key: string; label: string; state?: "loading" | "count"; count?: number }
  | { kind: "item"; key: string; group: string; hit?: Hit; label?: string; hint?: string }
  | { kind: "note"; key: string; text: string };

/** A place as the top bar shows it: answered (with its hits), still searching, or failed. */
export type PaletteGroup = {
  id: string;
  label: string;
  status: "loading" | "ok" | "error" | "off";
  hits?: Hit[];
  error?: string;
  failed?: PlaceAnswer["failed"];
};

/** The rows of the top bar's search, in order: Go to first (instant), then each place as it
 * answers: its top hits, "Searching…" while it has not, one muted line when it failed, and a muted
 * line naming workspaces it could not search. A place that found nothing is left out, so the list
 * holds only what was found ("No results in any place." when nothing was). Last, "See all
 * results". */
export function paletteRows(
  q: string,
  goTo: { key: string; label: string; hint: string }[],
  groups: PaletteGroup[],
  perPlace = 4,
): PaletteRow[] {
  const rows: PaletteRow[] = [];
  const text = q.trim();
  if (goTo.length) {
    rows.push({ kind: "group", key: "g:goto", label: "Go to" });
    for (const item of goTo) rows.push({ kind: "item", key: item.key, group: "goto", label: item.label, hint: item.hint });
  }
  if (!text) return rows;
  let shown = 0;
  for (const group of groups) {
    if (group.status === "off") continue;
    const hits = group.hits ?? [];
    if (group.status === "ok" && !hits.length && !group.failed?.length) continue;
    shown += 1;
    rows.push({
      kind: "group",
      key: `g:${group.id}`,
      label: group.label,
      ...(group.status === "loading" ? { state: "loading" as const } : group.status === "ok" ? { state: "count" as const, count: hits.length } : {}),
    });
    if (group.status === "loading") {
      rows.push({ kind: "note", key: `n:${group.id}`, text: "Searching…" });
    } else if (group.status === "error") {
      rows.push({ kind: "note", key: `n:${group.id}`, text: group.error ?? `Could not search ${group.label.toLowerCase()}.` });
    } else {
      for (const hit of topHits(hits, text, perPlace)) rows.push({ kind: "item", key: `${group.id}:${hit.key}`, group: group.id, hit });
      if (!hits.length) rows.push({ kind: "note", key: `n:${group.id}`, text: "No matches in the workspaces that answered." });
      if (group.failed?.length) rows.push({ kind: "note", key: `f:${group.id}`, text: `Could not search ${group.failed.map((f) => f.workspace).join(", ")}.` });
    }
  }
  if (!shown && groups.some((g) => g.status !== "off")) rows.push({ kind: "note", key: "n:none", text: "No results in any place." });
  rows.push({ kind: "item", key: "see-all", group: "all", label: `See all results for “${text}”` });
  return rows;
}

/** The items a keyboard moves across, in order. */
export function selectable(rows: PaletteRow[]): Extract<PaletteRow, { kind: "item" }>[] {
  return rows.filter((r): r is Extract<PaletteRow, { kind: "item" }> => r.kind === "item");
}

/** The item ↑ or ↓ moves to from `active` (by key, so answers arriving above keep the selection);
 * an unknown or missing `active` starts at the first. It stops at the ends. */
export function moveSelection(items: { key: string }[], active: string | null, step: 1 | -1): string | null {
  if (!items.length) return null;
  const at = items.findIndex((i) => i.key === active);
  if (at < 0) return items[0].key;
  return items[Math.max(0, Math.min(items.length - 1, at + step))].key;
}

/** The selected key: `active` while it is still shown. Until the person moves, the first page of
 * Go to, else "See all results": a default that never jumps while places answer, so Enter on a
 * query no page matches opens Data → Search. */
export function currentSelection(items: Extract<PaletteRow, { kind: "item" }>[], active: string | null): string | null {
  if (items.some((i) => i.key === active)) return active;
  return items.find((i) => i.group === "goto")?.key ?? items.find((i) => i.key === "see-all")?.key ?? items[0]?.key ?? null;
}

/** What ↑ or ↓ selects. Before the person has moved (`active` is null) the default may be See all
 * results at the foot; ↓ then starts at the first item rather than staying there. */
export function stepSelection(items: Extract<PaletteRow, { kind: "item" }>[], active: string | null, step: 1 | -1): string | null {
  const selected = currentSelection(items, active);
  if (active === null && step === 1 && selected === "see-all" && items[0]?.key !== "see-all") return items[0]?.key ?? null;
  return moveSelection(items, selected, step);
}
