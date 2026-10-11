/** What the organization, administration and search pages share: lake's `panel` sections, the
 * server's "meta" breadcrumb (rail group › rail item › the page's subject), GET filter forms that
 * become the URL, and the role and date cells. */
import type { FormEvent, ReactNode } from "react";
import { Link, useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { currentNav, navFor, sectionLabel } from "@/routes";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail, type Crumb } from "@/lib/trail";
import { dateTime, relative } from "@/lib/format";
import { Chip, Empty, cx } from "@/lib/ui";

/** Lake's `<section class="panel">` with an optional `<h2>`. */
export function Panel({ title, children, className }: { title?: ReactNode; children?: ReactNode; className?: string }) {
  return (
    <section className={cx("panel", className)}>
      {title && <h2>{title}</h2>}
      {children}
    </section>
  );
}

/** The empty state of a page-level list: the kit's `Empty`, compact. */
export function PanelEmpty({ children, title, action }: { children?: ReactNode; title?: ReactNode; action?: ReactNode }) {
  return (
    <Empty compact title={title} action={action}>
      {children}
    </Empty>
  );
}

/** The server's `default_trail(viewer, title, meta)`: the rail group (linked to its first item),
 * the rail item that owns this path, then `meta` (shortened when it is a long id) unless it repeats
 * the item. The document title stays the page's own. With no meta yet, the shell's default trail
 * shows. */
export function useMetaTrail(meta: string | null | undefined, title: string) {
  const session = useSession();
  const { pathname } = useLocation();
  let trail: Crumb[] | null = null;
  if (meta) {
    const items = navFor(session);
    const current = currentNav(pathname, items);
    trail = [];
    if (current && current.section !== "top") {
      const first = items.find((item) => item.section === current.section && !item.external);
      trail.push({ label: sectionLabel(current.section), to: first?.path });
    }
    if (current) trail.push({ label: current.label, to: current.path });
    if (!current || meta !== current.label) trail.push(entityCrumb(meta));
  }
  useTrail(trail, title);
}

/** One query parameter, "" when absent. */
export function useParam(name: string): string {
  const [params] = useSearchParams();
  return params.get(name) ?? "";
}

/** A GET filter form (lake's `<form class="filters" method="get">`): submitting it navigates to
 * `path` with the form's non-empty fields as the query, so the URL stays the page's state. */
export function useQuerySubmit(path: string) {
  const navigate = useNavigate();
  return (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const params = new URLSearchParams();
    for (const [key, value] of new FormData(event.currentTarget)) {
      if (typeof value === "string" && value !== "") params.append(key, value);
    }
    const query = params.toString();
    navigate(query ? `${path}?${query}` : path);
  };
}

/** A role (owner, admin, member, read, write…) as a chip: owners in the action colour, admins in
 * the AI colour, every other role plain. */
export function RoleBadge({ role }: { role: string | null | undefined }) {
  if (!role) return <span className="muted">—</span>;
  return <Chip tone={role === "owner" ? "action" : role === "admin" ? "ai" : "neutral"}>{role}</Chip>;
}
export const RoleChip = RoleBadge;

/** A stored timestamp, relative ("3h ago", "in 6d"), the full date and the stored value on hover. */
export function When({ value, empty = "—" }: { value: string | null | undefined; empty?: string }) {
  if (!value) return <span className="muted">{empty}</span>;
  return (
    <time dateTime={value} title={`${dateTime(value)} (${value})`} className="whitespace-nowrap">
      {relative(value)}
    </time>
  );
}

/** A table's primary cell: the name (a link when `to` is given) and a muted second line. */
export function NameCell({ to, name, sub, href }: { to?: string; href?: string; name: ReactNode; sub?: ReactNode }) {
  return (
    <div className="min-w-0">
      <div className="font-semibold text-ink">{to ? <Link to={to}>{name}</Link> : href ? <a href={href}>{name}</a> : name}</div>
      {sub !== undefined && sub !== null && sub !== "" && <div className="truncate text-[12px] text-ink-3">{sub}</div>}
    </div>
  );
}

/** An audited object (`repository/…`, `organization:…`) as a chip: its kind, then its id shortened,
 * the whole value on hover. */
export function ObjectChip({ value }: { value: string | null | undefined }) {
  if (!value) return <span className="muted">—</span>;
  const m = /^([a-z_][\w-]*)[/:](.+)$/i.exec(value);
  const kind = m?.[1];
  const id = m ? m[2] : value;
  return (
    <span className="inline-flex max-w-full items-center gap-1 rounded-full border border-rule bg-surface px-2 py-[1px] text-[12px] leading-5 text-ink-2" title={value}>
      {kind && <span className="text-ink-3">{kind.replace(/_/g, " ")}</span>}
      <span className="truncate font-mono text-[11.5px]">{id.length > 18 ? `${id.slice(0, 18)}…` : id}</span>
    </span>
  );
}

/** Tabs whose state is a query parameter (`?tab=`): links, the current one `aria-current`. */
export function QueryTabs({ tabs, label }: { tabs: { to: string; label: ReactNode; active: boolean; count?: number }[]; label: string }) {
  return (
    <nav className="tabs mb-4" aria-label={label}>
      {tabs.map((tab) => (
        <Link key={tab.to} to={tab.to} aria-current={tab.active ? "page" : undefined}>
          {tab.label}
          {tab.count !== undefined && <span className="tab-count">{tab.count}</span>}
        </Link>
      ))}
    </nav>
  );
}

/** Longer help, folded: "How it works" and the detail beneath it. */
export function HowItWorks({ children, summary = "How it works" }: { children: ReactNode; summary?: ReactNode }) {
  return (
    <details className="mt-4 text-[13px] text-ink-2">
      <summary className="cursor-pointer text-ink-3">{summary}</summary>
      <div className="mt-2 space-y-2">{children}</div>
    </details>
  );
}

/** Does any field contain the search text (case-insensitive)? An empty search matches. */
export function matches(search: string, ...fields: unknown[]): boolean {
  const needle = search.trim().toLowerCase();
  if (!needle) return true;
  return fields.some((f) => typeof f === "string" && f.toLowerCase().includes(needle));
}

/** Filter options from rows: each distinct value with its count, in first-seen order. */
export function countBy<T>(rows: T[] | undefined, key: (row: T) => string | null | undefined, label: (value: string) => ReactNode = (v) => v): { id: string; label: ReactNode; count: number }[] {
  const counts = new Map<string, number>();
  for (const row of rows ?? []) {
    const value = key(row);
    if (value) counts.set(value, (counts.get(value) ?? 0) + 1);
  }
  return [...counts.entries()].map(([id, count]) => ({ id, label: label(id), count }));
}

/** `a · b · c`: lake's inline link lists. */
export function Dotted({ children }: { children: ReactNode[] }) {
  return (
    <>
      {children.map((child, index) => (
        <span key={index}>
          {index > 0 && " · "}
          {child}
        </span>
      ))}
    </>
  );
}

/** The handle pattern lake checks for organization and team names. */
export const HANDLE_PATTERN = "[a-z0-9]([a-z0-9\\-]{0,254}[a-z0-9])?";
