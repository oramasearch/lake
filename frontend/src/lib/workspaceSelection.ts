// SPDX-License-Identifier: MIT

/** A multi-workspace selection, as Home's Dashboard and Usage keep it (lake#561, lake#564): the
 * URL's `?ws=<id>,<id>` first, else what this browser remembered, else every workspace. Ids that
 * are not among the viewer's workspaces are dropped. An empty answer means all of them. */
export function chosenWorkspaceIds(fromUrl: string | null, stored: unknown, known: string[]): string[] {
  if (fromUrl) return fromUrl.split(",").filter((id) => known.includes(id));
  if (Array.isArray(stored)) return stored.filter((id: unknown): id is string => typeof id === "string" && known.includes(id));
  return [];
}

/** What choosing `ids` stores and puts in the URL: nothing when it is every workspace. */
export function selectionToKeep(ids: string[], known: string[]): string[] {
  return ids.length === known.length ? [] : ids;
}

/** Toggling one workspace: from "all", it chooses that one alone; removing the last one goes back to
 * all. */
export function toggleWorkspace(current: string[], all: boolean, id: string): string[] {
  const from = all ? [] : current;
  return from.includes(id) ? from.filter((x) => x !== id) : [...from, id];
}
