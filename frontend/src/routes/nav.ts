/** The rail's rules that need no page code (lake#600): which rail item a path makes current, and the
 * Home group. Pure, so `npm test` runs them (tests/home.test.ts); `routes.tsx` joins them with the
 * page registry. */
import type { Audience, SectionKey } from "./types.ts";

/** One rail item. `external` opens a non-page URL instead of a page. */
export type NavItem = { section: SectionKey; path: string; label: string; icon: string; audience: Audience[]; external?: boolean; billing?: boolean };

/** Whether a rail item or page shows: a billing one only where the lake bills (`viewer.billing`,
 * lake#564); an enterprise install has billing off and never shows it. */
export function billingVisible(item: { billing?: boolean }, viewer: { billing?: boolean }): boolean {
  return !item.billing || viewer.billing === true;
}

/** The top of the rail. A signed-in person's Home group lists Ask Orama (the home, `/`), then
 * Dashboard, the whole platform at a glance (`/dashboard`). A visitor and the operator keep the one
 * Home item, their landing. */
export const HOME_NAV: NavItem[] = [
  { section: "top", path: "/", label: "Home", icon: "home", audience: ["visitor", "operator"] },
  { section: "home", path: "/", label: "Ask Orama", icon: "ask", audience: ["account"] },
  { section: "home", path: "/dashboard", label: "Dashboard", icon: "dashboard", audience: ["account"] },
];

const AO: Audience[] = ["account", "operator"];

/** The Data group: where a person's data lives and is found (Steve, 2026-10-10). Datasets is the
 * Train → Data page, kept at its address; Connections (lake#651) sits between it and Transcripts. */
export const DATA_NAV: NavItem[] = [
  { section: "data", path: "/repositories", label: "Repositories", icon: "repository", audience: ["visitor", "account", "operator"] },
  // A workspace's drives: files in a repository, as workspace files, or as shortcuts (lake#561).
  { section: "data", path: "/drives", label: "Drives", icon: "repository", audience: AO },
  { section: "data", path: "/shared-with-me", label: "Shared with Me", icon: "repository", audience: ["account"] },
  { section: "data", path: "/search", label: "Search", icon: "search", audience: AO },
  { section: "data", path: "/train/data", label: "Datasets", icon: "metering", audience: AO },
  // The databases a workspace reads, out of Datasets (lake#651).
  { section: "data", path: "/connections", label: "Connections", icon: "api", audience: AO },
  // Recordings, uploads and pasted text made into searchable transcripts (lake#561).
  { section: "data", path: "/transcripts", label: "Transcripts", icon: "audit", audience: AO },
];

/** Strip `/w/<id>` from a path: the workspace-local path the registry is keyed by. */
export function localPath(pathname: string): string {
  const match = /^\/w\/[^/]+(\/.*)?$/.exec(pathname);
  if (!match) return pathname;
  return match[1] && match[1] !== "/" ? match[1] : "/";
}

/** Whether a rail prefix covers a path: "/" only itself, any other prefix itself and below it. */
export function ownsPath(prefix: string, path: string): boolean {
  if (prefix === "/") return path === "/";
  return path === prefix || path.startsWith(prefix + "/");
}

/** The rail item current for a path: the first whose path, or a path its screen owns, covers it. */
export function currentNavIn(pathname: string, items: NavItem[], ownedBy: (path: string) => string[] = () => []): NavItem | undefined {
  const path = localPath(pathname);
  for (const item of items) {
    if (item.external) continue;
    const prefixes = [item.path, ...ownedBy(item.path)];
    if (prefixes.some((p) => ownsPath(p, path))) return item;
    if (item.path === "/design/projects" && path === "/design") return item;
  }
  return undefined;
}
