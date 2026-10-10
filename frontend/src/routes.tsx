/** The route registry: every page group's screens joined, the rail's items in lake's order, and the
 * lookups the shell, breadcrumbs and command palette share. */
import type { Session } from "@/lib/session";
import type { Audience, Screen, SectionKey } from "@/routes/types";
import { screens as account } from "@/pages/account/routes";
import { screens as admin } from "@/pages/admin/routes";
import { screens as apps } from "@/pages/apps/routes";
import { screens as ask } from "@/pages/ask/routes";
import { screens as design } from "@/pages/design/routes";
import { screens as developer } from "@/pages/developer/routes";
import { screens as experiments } from "@/pages/experiments/routes";
import { screens as home } from "@/pages/home/routes";
import { screens as organizations } from "@/pages/organizations/routes";
import { screens as repositories } from "@/pages/repositories/routes";
import { screens as search } from "@/pages/search/routes";
import { screens as ship } from "@/pages/ship/routes";
import { screens as streams } from "@/pages/streams/routes";
import { screens as train } from "@/pages/train/routes";
import { screens as transcripts } from "@/pages/transcripts/routes";
import { screens as workspaces } from "@/pages/workspaces/routes";

export type { Screen, SectionKey, Audience };

/** Every page. The catch-all ("*", home group) is last. */
export const SCREENS: Screen[] = [
  ...account,
  ...developer,
  ...repositories,
  ...search,
  ...workspaces,
  ...design,
  ...train,
  ...transcripts,
  ...experiments,
  ...streams,
  ...ship,
  ...apps,
  ...ask,
  ...organizations,
  ...admin,
  ...home,
].sort((a, b) => (a.path === "*" ? 1 : b.path === "*" ? -1 : 0));

const SECTION_LABELS: Record<SectionKey, string> = {
  top: "",
  data: "Data",
  design: "Design",
  train: "Train",
  ship: "Ship",
  organization: "Organization",
  administration: "Administration",
  developer: "Developer",
  account: "Account",
  none: "",
};

export function sectionLabel(key: SectionKey): string {
  return SECTION_LABELS[key];
}

/** One rail item. `external` opens a non-page URL instead of a page. */
export type NavItem = { section: SectionKey; path: string; label: string; icon: string; audience: Audience[]; external?: boolean };

const AO: Audience[] = ["account", "operator"];
const ALL: Audience[] = ["visitor", "account", "operator"];

/** The rail, in lake's order (web_ui.cpp nav_items). Organization is the account holder's group;
 * the operator sees Administration instead. */
export const NAV: NavItem[] = [
  { section: "top", path: "/", label: "Home", icon: "home", audience: ALL },
  // Data: where a person's data lives and is found (Steve, 2026-10-10). Datasets is the Train →
  // Data page, kept at its address.
  { section: "data", path: "/repositories", label: "Repositories", icon: "repository", audience: ALL },
  { section: "data", path: "/shared-with-me", label: "Shared with Me", icon: "repository", audience: ["account"] },
  { section: "data", path: "/search", label: "Search", icon: "search", audience: AO },
  { section: "data", path: "/train/data", label: "Datasets", icon: "metering", audience: AO },
  // Recordings, uploads and pasted text made into searchable transcripts (lake#561).
  { section: "data", path: "/transcripts", label: "Transcripts", icon: "audit", audience: AO },
  { section: "design", path: "/design/projects", label: "Projects", icon: "audit", audience: AO },
  { section: "design", path: "/design/opportunities", label: "Opportunities", icon: "search", audience: AO },
  { section: "design", path: "/design/solutions", label: "Solutions", icon: "audit", audience: AO },
  { section: "design", path: "/design/methodologies", label: "Methodologies", icon: "metering", audience: AO },
  { section: "design", path: "/design/agent-library", label: "Agent Library", icon: "agent", audience: AO },
  { section: "train", path: "/train/agents", label: "Agents", icon: "agent", audience: AO },
  { section: "train", path: "/train/runs", label: "Runs", icon: "metering", audience: AO },
  { section: "train", path: "/train/experiments", label: "Experiments", icon: "metering", audience: AO },
  { section: "train", path: "/train/streams", label: "Streams", icon: "streams", audience: AO },
  { section: "ship", path: "/ship/models", label: "Models", icon: "api", audience: AO },
  { section: "ship", path: "/ship/indexes", label: "Search Indexes", icon: "search", audience: AO },
  { section: "ship", path: "/ship/packages", label: "Packages", icon: "repository", audience: AO },
  { section: "ship", path: "/ship/apps", label: "Apps", icon: "api", audience: AO },
  { section: "ship", path: "/deployments", label: "Deployments", icon: "api", audience: AO },
  { section: "ship", path: "/services", label: "Services", icon: "api", audience: AO },
  { section: "ship", path: "/workspaces", label: "Workspaces", icon: "api", audience: AO },
  { section: "organization", path: "/organizations", label: "Organizations", icon: "organization", audience: ["account"] },
  { section: "organization", path: "/secret-mounts", label: "Secrets", icon: "secrets", audience: ["account"] },
  { section: "organization", path: "/metering", label: "Usage", icon: "metering", audience: ["account"] },
  { section: "organization", path: "/activity", label: "Activity", icon: "audit", audience: ["account"] },
  { section: "administration", path: "/customers", label: "Customers", icon: "customers", audience: ["operator"] },
  { section: "administration", path: "/organizations", label: "Organizations", icon: "organization", audience: ["operator"] },
  { section: "administration", path: "/applications", label: "Applications", icon: "api", audience: ["operator"] },
  { section: "administration", path: "/metering", label: "Usage", icon: "metering", audience: ["operator"] },
  { section: "administration", path: "/secret-mounts", label: "Secrets", icon: "secrets", audience: ["operator"] },
  { section: "administration", path: "/audit", label: "Audit", icon: "audit", audience: ["operator"] },
  { section: "developer", path: "/developer/api", label: "API", icon: "api", audience: AO },
  { section: "developer", path: "/developer/mcp", label: "MCP", icon: "api", audience: AO },
  { section: "developer", path: "/docs", label: "Docs", icon: "docs", audience: ALL },
];

export function audienceOf(session: Pick<Session, "viewer">): Audience {
  if (!session.viewer.signed_in) return "visitor";
  return session.viewer.administrator ? "operator" : "account";
}

export function navFor(session: Pick<Session, "viewer">): NavItem[] {
  const who = audienceOf(session);
  return NAV.filter((item) => item.audience.includes(who)).map((item) =>
    item.path === "/repositories" && who === "visitor" ? { ...item, label: "Public Repositories" } : item,
  );
}

/** Strip `/w/<id>` from a path: the workspace-local path the registry is keyed by. */
export function localPath(pathname: string): string {
  const match = /^\/w\/[^/]+(\/.*)?$/.exec(pathname);
  if (!match) return pathname;
  return match[1] && match[1] !== "/" ? match[1] : "/";
}

function owns(prefix: string, path: string): boolean {
  if (prefix === "/") return path === "/";
  return path === prefix || path.startsWith(prefix + "/");
}

/** The screen a page path belongs to (exact match first, then patterns with `:params`). */
export function matchScreen(pathname: string): Screen | undefined {
  const path = localPath(pathname);
  const exact = SCREENS.find((s) => s.path === path);
  if (exact) return exact;
  return SCREENS.find((s) => {
    if (!s.path.includes(":")) return false;
    const pattern = new RegExp("^" + s.path.replace(/:[^/]+/g, "[^/]+") + "$");
    return pattern.test(path);
  });
}

/** The rail item that is current for a path: the item whose path or owned paths cover it. The
 * first match wins, as on the server. */
export function currentNav(pathname: string, items: NavItem[]): NavItem | undefined {
  const path = localPath(pathname);
  for (const item of items) {
    if (item.external) continue;
    const screen = SCREENS.find((s) => s.path === item.path);
    const prefixes = [item.path, ...(screen?.owns ?? [])];
    if (prefixes.some((p) => owns(p, path))) return item;
    if (item.path === "/design/projects" && path === "/design") return item;
  }
  return undefined;
}

/** A workspace page's address under the current workspace. */
export function workspacePath(workspaceId: string | null, path: string): string {
  return workspaceId ? `/w/${encodeURIComponent(workspaceId)}${path}` : path;
}

/** Whether a page path is a workspace page (lake's workspace_page_path). */
export function isWorkspacePath(path: string): boolean {
  if (["/workspaces", "/workspaces/create", "/workspaces/delete", "/workspace/select"].includes(path)) return false;
  return ["/design", "/train", "/ship", "/ask", "/workspace", "/workspaces", "/transcripts", "/transcript"].some((p) => path === p || path.startsWith(p + "/"));
}

/** The pages the palette offers: rail items the viewer can open, plus account pages. */
export function navigableScreens(session: Pick<Session, "viewer">): Screen[] {
  const items = navFor(session).filter((item) => !item.external);
  const seen = new Set<string>();
  const out: Screen[] = [];
  for (const item of items) {
    const screen = SCREENS.find((s) => s.path === item.path);
    if (screen && !seen.has(screen.path)) {
      seen.add(screen.path);
      out.push({ ...screen, title: item.label, section: item.section });
    }
  }
  if (session.viewer.signed_in && !session.viewer.administrator) {
    for (const path of ["/settings", "/sessions", "/tokens", "/ask"]) {
      const screen = SCREENS.find((s) => s.path === path);
      if (screen) out.push(screen);
    }
  }
  return out;
}

/** A link to a capability-free route that is not part of the app (downloads, the OpenAPI document,
 * protocol routes). The router must never handle these. */
export function isServerRoute(path: string): boolean {
  return /^\/(v1|mcp|fonts|ui|assets|scim|acme|v2|buildid|_lake|legacy|simple|packages|api|healthz|readyz|metrics|llms|skills|style)/.test(path) ||
    /^\/(file\/(raw|download)|drive\/download|exports\/download|share\/[^/]+\/download|developer\/mcp\/skill|apps\/)/.test(path);
}
