// SPDX-License-Identifier: MIT

/** What the top bar's workspace picker shows (components/WorkspacePicker.tsx, lake#638): who owns
 * each group of workspaces, what the pill says, when the filter box appears and what it keeps. Pure,
 * so `npm test` covers it. */

/** The parts of a space (customer) the picker reads; `SpaceChoice` in lib/session.tsx. */
export type PickerSpace = {
  id: string;
  name: string;
  label: string;
  personal: boolean;
  kind?: string;
  workspaces: { id: string; name: string; customer_id: string; status?: string }[];
};

/** The parts of the viewer the picker reads. */
export type PickerViewer = { display_name?: string; email?: string; handle?: string };

/** More workspaces than this, across every owner, and the menu opens with a filter box. */
export const FILTER_AFTER = 8;

/** The small label over a group's name. */
export function ownerKind(space: PickerSpace): "Your account" | "Organization" | "Account" {
  if (space.personal) return "Your account";
  return space.kind === "organization" ? "Organization" : "Account";
}

/** A group's name as written: the person for their own account (never the generated handle),
 * else the owner's name. */
export function ownerName(space: PickerSpace, viewer: PickerViewer): string {
  if (space.personal) return viewer.display_name?.trim() || viewer.email?.trim() || "You";
  return space.label?.trim() || space.name;
}

/** The owner's name in the pill, beside the workspace name: short, since the menu says the rest. */
export function pillOwner(space: PickerSpace): string {
  return space.personal ? "Your account" : space.label?.trim() || space.name;
}

/** The letter in an owner's avatar: its first letter or digit, upper-cased. */
export function initialOf(name: string): string {
  const first = [...name.trim()].find((c) => /[\p{L}\p{N}]/u.test(c));
  return first ? first.toLocaleUpperCase() : "?";
}

/** The pill names the owner only when the viewer has more than one. */
export function showsOwner(spaces: PickerSpace[]): boolean {
  return spaces.length > 1;
}

/** Every workspace the viewer can choose, across owners. */
export function workspaceCount(spaces: PickerSpace[]): number {
  return spaces.reduce((n, s) => n + s.workspaces.length, 0);
}

/** Whether the menu opens with a filter box. */
export function needsFilter(spaces: PickerSpace[]): boolean {
  return workspaceCount(spaces) > FILTER_AFTER;
}

/** The pill's full text, its tooltip when the name is cut short. */
export function pillTitle(workspace: string, space: PickerSpace | null | undefined, viewer: PickerViewer, many: boolean): string {
  if (!space || !many) return workspace;
  return `${workspace} — ${ownerName(space, viewer)} (${ownerKind(space)})`;
}

/** The groups a filter keeps. With no filter every owner shows, even one with no workspaces; with
 * one, a workspace stays when its name or its owner's name contains the words, and an owner with
 * nothing left is dropped. Case and accents are ignored. */
export function filterGroups<S extends PickerSpace>(spaces: S[], viewer: PickerViewer, filter: string): S[] {
  const words = fold(filter).split(/\s+/).filter(Boolean);
  if (!words.length) return spaces;
  const out: S[] = [];
  for (const space of spaces) {
    const owner = fold(`${ownerName(space, viewer)} ${space.name} ${ownerKind(space)}`);
    const kept = space.workspaces.filter((w) => {
      const text = `${fold(w.name)} ${owner}`;
      return words.every((word) => text.includes(word));
    });
    if (kept.length) out.push({ ...space, workspaces: kept });
  }
  return out;
}

/** The workspace Enter in the filter box opens: the first one left. */
export function firstMatch(groups: PickerSpace[]): string | null {
  for (const g of groups) if (g.workspaces.length) return g.workspaces[0].id;
  return null;
}

/** Where "New workspace" goes: Workspaces with its dialog open, for the current owner, where the
 * dialog asks which owner when there is more than one. */
export function newWorkspacePath(spaceId: string | null | undefined): string {
  const q = new URLSearchParams();
  if (spaceId) q.set("customer_id", spaceId);
  q.set("new", "1");
  return `/workspaces?${q.toString()}`;
}

function fold(text: string): string {
  return text.normalize("NFD").replace(/\p{M}/gu, "").toLocaleLowerCase();
}

/** The owners in the order the picker and Workspaces show them: the person's own account first,
 * then each organization in the order lake answered them. */
export function orderOwners<S extends PickerSpace>(spaces: S[]): S[] {
  return [...spaces.filter((s) => s.personal), ...spaces.filter((s) => !s.personal)];
}

/** Which workspace the app is in, and what to fix when the one asked for is gone (lake#638).
 * `fromPath` is the URL's `/w/<id>`, `remembered` this browser's `lake-workspace`, `suggested` the
 * `viewer` read's `workspace_id`. A live `fromPath` wins. Otherwise: the suggested one if it is
 * live, else the remembered one, else the person's own first workspace, else the first one any
 * owner has; none only when the person can reach no workspace at all. `replace` says the URL names
 * a workspace that no longer exists and must be rewritten to `id`; `remember` is what to store in
 * place of a stale remembered workspace. */
export function resolveWorkspace(
  spaces: PickerSpace[],
  { fromPath, remembered, suggested }: { fromPath: string | null; remembered: string | null; suggested?: string | null },
): { id: string | null; replace: boolean; remember: string | null } {
  const live = new Set(spaces.flatMap((s) => s.workspaces.map((w) => w.id)));
  if (fromPath && live.has(fromPath)) return { id: fromPath, replace: false, remember: null };
  const first = orderOwners(spaces).find((s) => s.workspaces.length)?.workspaces[0]?.id ?? null;
  const id = (suggested && live.has(suggested) ? suggested : null) ?? (remembered && live.has(remembered) ? remembered : null) ?? first;
  return { id, replace: !!fromPath && !!id, remember: id && remembered !== id ? id : null };
}

/** A workspace row of Workspaces (`list_workspaces`), as much of it as the grouping reads. */
export type ListedWorkspace = { id: string; name: string; desired?: string; phase?: string; storage_gi?: number };

/** Workspaces' groups: every owner the picker shows, in its order, each with its workspaces in the
 * picker's order (`list_workspaces` supplies the details). A workspace being deleted is gone from
 * the picker and from here; one lake listed that the viewer read has not seen yet comes last. An
 * owner whose list has not answered yet has `rows: null`. */
export function workspaceGroups<S extends PickerSpace, W extends ListedWorkspace>(
  spaces: S[],
  listed: Record<string, W[] | undefined>,
): { space: S; rows: W[] | null }[] {
  return orderOwners(spaces).map((space) => {
    const answered = listed[space.id];
    if (!answered) return { space, rows: null };
    const live = answered.filter((w) => w.desired !== "deleted");
    const byId = new Map(live.map((w) => [w.id, w]));
    const ordered = space.workspaces.map((w) => byId.get(w.id)).filter((w): w is W => !!w);
    const known = new Set(space.workspaces.map((w) => w.id));
    return { space, rows: [...ordered, ...live.filter((w) => !known.has(w.id))] };
  });
}

/** The phase a Workspaces row shows: a requested deletion first, then the operator's phase. */
export function phaseOf(row: ListedWorkspace): string {
  return row.desired === "deleted" ? "deleting" : row.phase || "pending";
}

/** Workspaces' figures, over every workspace of every owner. */
export function workspaceFigures(rows: ListedWorkspace[]): { workspaces: number; ready: number; failed: number; storage: number } {
  return {
    workspaces: rows.length,
    ready: rows.filter((r) => phaseOf(r) === "Ready").length,
    failed: rows.filter((r) => phaseOf(r) === "Failed").length,
    storage: rows.reduce((sum, r) => sum + (typeof r.storage_gi === "number" ? r.storage_gi : 0), 0),
  };
}
