/** What the organization, administration and search pages share: lake's `panel` sections, the
 * server's "meta" breadcrumb (rail group › rail item › the page's subject), GET filter forms that
 * become the URL, and the role and date cells. */
import type { FormEvent, ReactNode } from "react";
import { useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { currentNav, navFor, sectionLabel } from "@/routes";
import { useSession } from "@/lib/session";
import { entityCrumb, useTrail, type Crumb } from "@/lib/trail";
import { dateTime } from "@/lib/format";
import { Badge, cx } from "@/lib/ui";

/** Lake's `<section class="panel">` with an optional `<h2>`. */
export function Panel({ title, children, className }: { title?: ReactNode; children?: ReactNode; className?: string }) {
  return (
    <section className={cx("panel", className)}>
      {title && <h2>{title}</h2>}
      {children}
    </section>
  );
}

/** Lake's `<div class="panel empty">`: the empty state of a page-level list. */
export function PanelEmpty({ children }: { children: ReactNode }) {
  return <div className="panel empty">{children}</div>;
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

/** A role (owner, admin, member, read, write…) as lake showed it: a plain badge. */
export function RoleBadge({ role }: { role: string | null | undefined }) {
  if (!role) return <span className="muted">—</span>;
  return <Badge>{role}</Badge>;
}

/** A stored timestamp in words, the stored value on hover. */
export function When({ value }: { value: string | null | undefined }) {
  if (!value) return <span className="muted">—</span>;
  return <time dateTime={value} title={value}>{dateTime(value)}</time>;
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
