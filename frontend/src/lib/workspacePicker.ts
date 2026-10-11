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

/** A group's name as written: the person for their own account (never the generated handle, Steve
 * 2026-10-10), else the owner's name. */
export function ownerName(space: PickerSpace, viewer: PickerViewer): string {
  if (space.personal) return viewer.display_name?.trim() || viewer.email?.trim() || "You";
  return space.label?.trim() || space.name;
}

/** The owner's name in the pill, beside the workspace name: short, since the menu says the rest. */
export function pillOwner(space: PickerSpace): string {
  return space.personal ? "Personal" : space.label?.trim() || space.name;
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
