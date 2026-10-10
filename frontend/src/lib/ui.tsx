/** The UI kit: the components every screen is made of, so no screen invents its own. They render
 * lake's existing component classes (styles/lake.css), so the look is lake's; Tailwind utilities,
 * bound to the same tokens, only arrange them. */
import {
  type ComponentType,
  Children,
  cloneElement,
  isValidElement,
  type ReactElement,
  type ReactNode,
  useId,
  useMemo,
  useState,
} from "react";
import { Link, NavLink } from "react-router-dom";
import type { UseQueryResult } from "@tanstack/react-query";
import { Check, Copy, Inbox, Loader2 } from "lucide-react";
import { ApiError, isSignedIn } from "./api";

export function cx(...parts: unknown[]): string {
  return parts.filter(Boolean).join(" ");
}

/* ------------------------------------------------------------------ page structure */

/** A page: eyebrow, Title Case heading, one sentence of purpose, actions, then its content. */
export function Page({
  eyebrow,
  title,
  sub,
  actions,
  children,
  wide,
}: {
  eyebrow?: ReactNode;
  title: ReactNode;
  sub?: ReactNode;
  actions?: ReactNode;
  children?: ReactNode;
  wide?: boolean;
}) {
  return (
    <div className={cx("page-container", wide && "page-wide")}>
      <header className="page-header">
        <div className="min-w-0">
          {eyebrow && <p className="eyebrow">{eyebrow}</p>}
          <h1>{title}</h1>
          {sub && <p className="sub">{sub}</p>}
        </div>
        {actions && <div className="page-actions">{actions}</div>}
      </header>
      {children}
    </div>
  );
}

/** A titled section of a page (an `<h2>`, optional actions on its right). */
export function Section({
  title,
  sub,
  actions,
  children,
  id,
  level = 2,
}: {
  title?: ReactNode;
  sub?: ReactNode;
  actions?: ReactNode;
  children?: ReactNode;
  id?: string;
  level?: 2 | 3;
}) {
  const Heading = level === 2 ? "h2" : "h3";
  return (
    <section id={id} className="page-section">
      {(title || actions) && (
        <div className="section-heading">
          {title && <Heading>{title}</Heading>}
          {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
        </div>
      )}
      {sub && <p className="muted mt-0">{sub}</p>}
      {children}
    </section>
  );
}

/** A card (rev-os's): a hairline 6px panel with an optional mono `label`, a `title`, actions on the
 * right and a footer. `tone` marks an exception (danger edge), the active item (action edge) or a
 * quiet one; `to` makes the whole card a link. */
export function Card({
  children,
  className,
  title,
  label,
  actions,
  footer,
  tone = "default",
  padded = true,
  to,
}: {
  children?: ReactNode;
  className?: string;
  title?: ReactNode;
  label?: ReactNode;
  actions?: ReactNode;
  footer?: ReactNode;
  tone?: "default" | "exception" | "active" | "quiet";
  padded?: boolean;
  to?: string;
}) {
  const body = (
    <div
      className={cx(
        "rounded-[6px] border bg-surface",
        tone === "exception" && "border-hairline border-l-[3px] border-l-bad",
        tone === "active" && "border-hairline border-l-[3px] border-l-action",
        tone === "quiet" && "border-hairline bg-transparent",
        tone === "default" && "border-hairline",
        to && "transition-colors hover:border-rule hover:bg-raised/40",
        className,
      )}
    >
      {(title || label || actions) && (
        <div className={cx("flex flex-wrap items-start gap-x-3 gap-y-2", padded ? "px-4 pt-3" : "border-b border-hairline px-4 py-3")}>
          <div className="min-w-0 flex-1">
            {label && <div className="label m-0">{label}</div>}
            {title && <div className="mt-0.5 text-[14.5px] font-semibold text-ink">{title}</div>}
          </div>
          {actions && <div className="flex shrink-0 items-center gap-1.5">{actions}</div>}
        </div>
      )}
      {children !== undefined && <div className={cx(padded && "px-4 pt-2 pb-3.5")}>{children}</div>}
      {footer && <div className="border-t border-hairline px-4 py-2 text-[12.5px] text-ink-2">{footer}</div>}
    </div>
  );
  return to ? (
    <Link to={to} className="block text-inherit no-underline">
      {body}
    </Link>
  ) : (
    body
  );
}

/** A figure tile: a mono label, a large number and a line beneath (rev-os's Stat). */
export function Stat({
  label,
  value,
  sub,
  tone,
  icon: IconC,
  to,
}: {
  label: ReactNode;
  value: ReactNode;
  sub?: ReactNode;
  tone?: "bad" | "ok";
  icon?: ComponentType<{ size?: number; className?: string }>;
  to?: string;
}) {
  const inner = (
    <div className={cx("h-full rounded-[6px] border border-hairline bg-surface px-4 py-3", tone === "bad" && "border-l-[3px] border-l-bad", to && "transition-colors hover:border-rule")}>
      <div className="flex items-center gap-2">
        {IconC && <IconC size={13} className="text-ink-3" />}
        <div className="label m-0 truncate">{label}</div>
      </div>
      <div className={cx("mt-1.5 font-mono text-[22px] leading-none font-semibold tabular-nums text-ink", tone === "bad" && "text-bad", tone === "ok" && "text-ok")}>{value}</div>
      {sub && <div className="mt-1.5 text-[12px] text-ink-3">{sub}</div>}
    </div>
  );
  return to ? (
    <Link to={to} className="block h-full text-inherit no-underline">
      {inner}
    </Link>
  ) : (
    inner
  );
}

/** A row of figure tiles: 2 to 4 across on wide screens. */
export function StatRow({ children, cols = 4 }: { children: ReactNode; cols?: 2 | 3 | 4 }) {
  return <div className={cx("mb-5 grid grid-cols-1 gap-3 sm:grid-cols-2", cols === 3 && "lg:grid-cols-3", cols === 4 && "lg:grid-cols-4")}>{children}</div>;
}

/** Main and side columns (rev-os's Split). */
export function Split({ children, side, ratio = "wide" }: { children: ReactNode; side: ReactNode; ratio?: "wide" | "even" }) {
  return (
    <div className={cx("grid grid-cols-1 gap-4", ratio === "wide" ? "xl:grid-cols-[minmax(0,1.55fr)_minmax(0,1fr)]" : "xl:grid-cols-2")}>
      <div className="min-w-0 space-y-4">{children}</div>
      <div className="min-w-0 space-y-4">{side}</div>
    </div>
  );
}

/** A pill (rev-os's Chip): a tag, a count or, with `onClick`, a filter. */
export function Chip({
  children,
  tone = "neutral",
  onClick,
  active,
  title,
}: {
  children: ReactNode;
  tone?: "neutral" | "action" | "ok" | "bad" | "warn" | "ai";
  onClick?: () => void;
  active?: boolean;
  title?: string;
}) {
  const className = cx(
    "inline-flex items-center gap-1 whitespace-nowrap rounded-full border px-2.5 py-[1px] text-[12px] leading-5",
    tone === "neutral" && "border-rule bg-surface text-ink-2",
    tone === "action" && "border-action/40 bg-action-soft text-action",
    tone === "ok" && "border-transparent bg-ok-soft text-ok",
    tone === "bad" && "border-transparent bg-bad-soft text-bad",
    tone === "warn" && "border-transparent bg-warn-soft text-warn",
    tone === "ai" && "border-transparent bg-ai-soft text-ai",
    onClick && "cursor-pointer hover:border-action",
    active && "border-action font-semibold text-action",
  );
  return onClick ? (
    <button type="button" className={cx(className, "chip-button")} onClick={onClick} aria-pressed={active} title={title}>
      {children}
    </button>
  ) : (
    <span className={className} title={title}>
      {children}
    </span>
  );
}

/** Filter pills: All, then each option with its count. */
export function FilterChips<T extends string>({
  options,
  value,
  onChange,
  allLabel = "All",
}: {
  options: { id: T; label: ReactNode; count?: number }[];
  value: T | null;
  onChange: (v: T | null) => void;
  allLabel?: string;
}) {
  return (
    <div className="flex flex-wrap items-center gap-1.5">
      <Chip onClick={() => onChange(null)} active={value === null}>
        {allLabel}
      </Chip>
      {options.map((o) => (
        <Chip key={o.id} onClick={() => onChange(value === o.id ? null : o.id)} active={value === o.id}>
          {o.label}
          {o.count != null && <span className="font-mono text-[10.5px] text-ink-3">{o.count}</span>}
        </Chip>
      ))}
    </div>
  );
}

/** The row above a list: search, filters and actions. */
export function Toolbar({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cx("mb-3 flex flex-wrap items-center gap-2", className)}>{children}</div>;
}

export function SearchInput({ value, onChange, placeholder = "Search…", label = "Search" }: { value: string; onChange: (v: string) => void; placeholder?: string; label?: string }) {
  return <input className="search-input" type="search" aria-label={label} value={value} onChange={(e) => onChange(e.target.value)} placeholder={placeholder} />;
}

/** A coloured edge and a sentence: information, a warning or a success (rev-os's Banner). */
export function Banner({ tone = "info", title, children, action }: { tone?: "info" | "warn" | "ok" | "quiet"; title?: ReactNode; children?: ReactNode; action?: ReactNode }) {
  return (
    <div
      className={cx(
        "mb-3 flex items-start gap-3 rounded-[5px] border-l-[3px] px-3.5 py-2.5 text-[13px]",
        tone === "info" && "border-l-action bg-action-soft text-ink",
        tone === "warn" && "border-l-bad bg-bad-soft text-bad",
        tone === "ok" && "border-l-ok bg-ok-soft text-ok",
        tone === "quiet" && "border-l-rule bg-raised text-ink-2",
      )}
      role={tone === "warn" ? "alert" : "status"}
    >
      <div className="min-w-0 flex-1">
        {title && <div className="font-semibold">{title}</div>}
        {children && <div className={cx(title && "mt-0.5", tone === "warn" ? "text-bad" : "text-ink-2")}>{children}</div>}
      </div>
      {action && <div className="shrink-0">{action}</div>}
    </div>
  );
}

/** A track and its fill, with the value beside it. */
export function Meter({ value, max = 100, label, tone }: { value: number | null | undefined; max?: number; label?: ReactNode; tone?: "bad" | "ok" }) {
  const v = value ?? 0;
  const w = Math.max(0, Math.min(100, max > 0 ? (v / max) * 100 : 0));
  return (
    <div className="flex items-center gap-2">
      <div className="h-2 flex-1 overflow-hidden rounded-full bg-raised">
        <div className={cx("h-full rounded-full", tone === "bad" ? "bg-bad" : tone === "ok" ? "bg-ok" : "bg-action")} style={{ width: `${w}%` }} />
      </div>
      {label !== undefined && <span className="font-mono text-[11.5px] text-ink-2 tabular-nums">{label}</span>}
    </div>
  );
}

export function Grid({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cx("grid", className)}>{children}</div>;
}

/** Label / value pairs as a definition list. Values that are absent read "—". */
export function KeyValue({ rows, className }: { rows: [ReactNode, ReactNode][]; className?: string }) {
  return (
    <dl className={className}>
      {rows.map(([label, value], index) => (
        <div key={index} className="contents">
          <dt>{label}</dt>
          <dd>{value === undefined || value === null || value === "" ? "—" : value}</dd>
        </div>
      ))}
    </dl>
  );
}

/* ------------------------------------------------------------------ controls */

type ButtonProps = {
  children?: ReactNode;
  variant?: "secondary" | "primary" | "danger" | "ghost";
  onClick?: (e: React.MouseEvent<HTMLButtonElement>) => void;
  type?: "button" | "submit";
  disabled?: boolean;
  loading?: boolean;
  to?: string;
  href?: string;
  title?: string;
  className?: string;
  "aria-label"?: string;
  form?: string;
};

/** A button, or a link styled as one (`to` for an app route, `href` for a download). */
export function Button({ children, variant = "secondary", onClick, type = "button", disabled, loading, to, href, title, className, form, ...rest }: ButtonProps) {
  const classes = cx(
    "button",
    variant === "primary" && "button-primary",
    variant === "danger" && "button-danger",
    variant === "ghost" && "button-ghost",
    className,
  );
  const content = (
    <>
      {loading && <Loader2 size={14} className="animate-spin" aria-hidden />}
      {children}
    </>
  );
  if (to && !disabled) {
    return (
      <Link to={to} className={classes} title={title} aria-label={rest["aria-label"]}>
        {content}
      </Link>
    );
  }
  if (href && !disabled) {
    return (
      <a href={href} className={classes} title={title} aria-label={rest["aria-label"]}>
        {content}
      </a>
    );
  }
  return (
    <button type={type} className={classes} onClick={onClick} disabled={disabled || loading} title={title} aria-label={rest["aria-label"]} form={form}>
      {content}
    </button>
  );
}

/** A row of actions under a page heading or inside a card. */
export function Actions({ children, className }: { children: ReactNode; className?: string }) {
  return <div className={cx("actions", className)}>{children}</div>;
}

/** Tabs that are routes. `end` matches the path exactly. */
export function Tabs({ tabs, label = "Sections" }: { tabs: { to: string; label: ReactNode; end?: boolean; count?: number }[]; label?: string }) {
  return (
    <nav className="tabs mb-4" aria-label={label}>
      {tabs.map((tab) => (
        // NavLink marks the active tab aria-current="page", which lake's .tabs style reads.
        <NavLink key={tab.to} to={tab.to} end={tab.end}>
          {tab.label}
          {tab.count !== undefined && <span className="tab-count">{tab.count}</span>}
        </NavLink>
      ))}
    </nav>
  );
}

/** Tabs held in state (a view switch inside one page). */
export function StateTabs<T extends string>({ value, onChange, tabs, label = "View" }: { value: T; onChange: (v: T) => void; tabs: { value: T; label: ReactNode }[]; label?: string }) {
  return (
    <div className="view-toggle mb-4" role="group" aria-label={label}>
      {tabs.map((tab) => (
        <button key={tab.value} type="button" aria-pressed={value === tab.value} onClick={() => onChange(tab.value)}>
          {tab.label}
        </button>
      ))}
    </div>
  );
}

/* ------------------------------------------------------------------ status */

export type Tone = "neutral" | "success" | "warning" | "danger" | "ai" | "info";

/** A status badge: word + dot + colour, never colour alone. */
export function Badge({ children, tone = "neutral", title }: { children: ReactNode; tone?: Tone; title?: string }) {
  return (
    <span className={cx("badge", tone !== "neutral" && tone !== "info" && `status-${tone}`, tone === "info" && "status-info")} title={title}>
      {children}
    </span>
  );
}

/** The tone a lifecycle word carries, for the statuses lake's capabilities answer. */
export function toneOf(status: string | null | undefined): Tone {
  const s = (status ?? "").toLowerCase();
  if (/^(ready|running|active|succeeded|success|healthy|ok|done|complete|completed|connected|enabled|verified|passed|approved|live|deployed|available|open)$/.test(s)) return "success";
  if (/^(failed|failure|error|errored|unhealthy|revoked|denied|rejected|blocked|crashed|deleted|disabled|expired|cancelled|canceled)$/.test(s)) return "danger";
  if (/^(pending|queued|starting|creating|provisioning|building|waiting|stopping|deleting|degraded|paused|draft|in_progress|scheduled|not_ready|unknown)$/.test(s)) return "warning";
  return "neutral";
}

/** A status as a badge in words ("not_ready" → "Not ready"). */
/** A status in words, as rev-os's state chip: a glyph, the word and its colour (never colour alone). */
export function Status({ value, tone }: { value: string | null | undefined; tone?: Tone }) {
  if (!value) return <span className="muted">—</span>;
  const words = value.replace(/[_-]+/g, " ");
  const t = tone ?? toneOf(value);
  const glyph = t === "success" ? "✓" : t === "danger" ? "✗" : t === "warning" ? "○" : t === "ai" ? "◆" : "·";
  return (
    <span
      className={cx(
        "inline-flex items-center gap-1 rounded-[3px] px-1.5 py-[1px] font-mono text-[10.5px] leading-4 whitespace-nowrap",
        t === "success" && "bg-ok-soft text-ok",
        t === "danger" && "bg-bad-soft text-bad",
        t === "warning" && "bg-warn-soft text-warn",
        t === "ai" && "bg-ai-soft text-ai",
        t === "info" && "bg-action-soft text-action",
        t === "neutral" && "bg-raised text-ink-2",
      )}
    >
      <span aria-hidden>{glyph}</span>
      {words.charAt(0).toUpperCase() + words.slice(1)}
    </span>
  );
}

export function Notice({ children, tone = "info", title }: { children: ReactNode; tone?: "info" | "danger" | "warning" | "success"; title?: ReactNode }) {
  return (
    <div className={cx(tone === "danger" ? "error" : "notice", tone === "warning" && "notice-warning", tone === "success" && "notice-success")} role={tone === "danger" ? "alert" : "status"}>
      {title && <strong className="block">{title}</strong>}
      {children}
    </div>
  );
}

/** Nothing here yet: one sentence and the next action. */
/** Nothing here yet (rev-os's empty state): an icon, a title, one sentence and the next action. */
export function Empty({
  children,
  action,
  compact,
  title,
  icon: IconC = Inbox,
}: {
  children?: ReactNode;
  action?: ReactNode;
  compact?: boolean;
  title?: ReactNode;
  icon?: ComponentType<{ size?: number }>;
}) {
  return (
    <div className={cx("flex flex-col items-center justify-center rounded-[6px] border border-dashed border-rule text-center", compact ? "px-4 py-6" : "px-6 py-12")}>
      <div className="mb-2 flex h-9 w-9 items-center justify-center rounded-full bg-raised text-ink-3">
        <IconC size={17} />
      </div>
      {title && <div className="text-[14px] font-semibold text-ink">{title}</div>}
      {children && <div className={cx("max-w-md text-[13px] text-ink-2", title && "mt-1")}>{children}</div>}
      {action && <div className="mt-3 flex justify-center gap-2">{action}</div>}
    </div>
  );
}

export function Skeleton({ lines = 3 }: { lines?: number }) {
  return (
    <div className="loading-bars" aria-busy="true" aria-label="Loading">
      {Array.from({ length: lines }, (_, i) => (
        <span key={i} />
      ))}
    </div>
  );
}

/** A failed read, in words, with Retry. A failed read never looks like an empty list. */
export function ErrorBlock({ error, retry, what }: { error: unknown; retry?: () => void; what?: string }) {
  const e = error instanceof ApiError ? error : null;
  const title =
    e?.status === 403
      ? "You don't have access to this"
      : e?.status === 404
        ? "Not found"
        : e?.status === 401 && isSignedIn()
          ? "Your session has ended"
          : what
            ? `Couldn't load ${what}`
            : "Couldn't load this";
  const message = e?.message ?? (error instanceof Error ? error.message : "The request failed.");
  return (
    <div className="error" role="alert">
      <strong className="block">{title}</strong>
      <span>{message}</span>
      {retry && e?.status !== 403 && e?.status !== 404 && (
        <div className="mt-2">
          <Button onClick={retry}>Retry</Button>
        </div>
      )}
    </div>
  );
}

/** Loading, error, empty and data in one line: `<Query q={q} what="repositories">{(v) => …}</Query>`. */
export function Query<T>({
  q,
  children,
  what,
  empty,
  isEmpty,
  lines,
}: {
  q: UseQueryResult<T>;
  children: (value: T) => ReactNode;
  what?: string;
  empty?: ReactNode;
  isEmpty?: (value: T) => boolean;
  lines?: number;
}) {
  if (q.isLoading) return <Skeleton lines={lines} />;
  if (q.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what={what} />;
  if (q.data === undefined) return null;
  const blank = isEmpty ? isEmpty(q.data) : Array.isArray(q.data) && q.data.length === 0;
  if (blank && empty !== undefined) return <>{typeof empty === "string" ? <Empty>{empty}</Empty> : empty}</>;
  return <>{children(q.data)}</>;
}

/* ------------------------------------------------------------------ tables */

export type Column<T> = {
  key: string;
  label: ReactNode;
  render?: (row: T) => ReactNode;
  /** The value sorted by; a column without one does not sort. */
  sort?: (row: T) => string | number | null | undefined;
  className?: string;
  numeric?: boolean;
};

/** A table. With `q` it shows the skeleton while loading and the error on a failed read before it
 * considers `empty`, so an outage never reads as "nothing here". */
export function DataTable<T>({
  rows,
  columns,
  q,
  what,
  empty,
  rowKey,
  caption,
  initialSort,
}: {
  rows: T[] | undefined;
  columns: Column<T>[];
  q?: Pick<UseQueryResult<unknown>, "isLoading" | "isError" | "error" | "refetch">;
  what?: string;
  empty?: ReactNode;
  rowKey?: (row: T, index: number) => string;
  caption?: string;
  initialSort?: { key: string; dir: "ascending" | "descending" };
}) {
  const [sort, setSort] = useState(initialSort ?? null);
  const sorted = useMemo(() => {
    if (!rows || !sort) return rows ?? [];
    const column = columns.find((c) => c.key === sort.key);
    if (!column?.sort) return rows;
    const by = column.sort;
    return [...rows].sort((a, b) => {
      const x = by(a);
      const y = by(b);
      if (x === y) return 0;
      if (x === null || x === undefined) return 1;
      if (y === null || y === undefined) return -1;
      const order = x < y ? -1 : 1;
      return sort.dir === "ascending" ? order : -order;
    });
  }, [rows, sort, columns]);
  if (q?.isLoading) return <Skeleton />;
  if (q?.isError) return <ErrorBlock error={q.error} retry={() => void q.refetch()} what={what} />;
  if (!sorted.length && empty !== undefined) return <>{typeof empty === "string" ? <Empty>{empty}</Empty> : empty}</>;
  return (
    <div className="table-wrap" role="region" aria-label={caption ?? what} tabIndex={0}>
      <table>
        {caption && <caption className="sr-only">{caption}</caption>}
        <thead>
          <tr>
            {columns.map((column) => {
              const active = sort?.key === column.key;
              return (
                <th key={column.key} scope="col" aria-sort={active ? sort!.dir : undefined} className={cx(column.numeric && "text-right", column.className)}>
                  {column.sort ? (
                    <button
                      type="button"
                      className="th-sort"
                      onClick={() => setSort({ key: column.key, dir: active && sort!.dir === "ascending" ? "descending" : "ascending" })}
                    >
                      {column.label}
                    </button>
                  ) : (
                    column.label
                  )}
                </th>
              );
            })}
          </tr>
        </thead>
        <tbody>
          {sorted.map((row, index) => (
            <tr key={rowKey ? rowKey(row, index) : index}>
              {columns.map((column) => (
                <td key={column.key} className={cx(column.numeric && "text-right tabular-nums", column.className)}>
                  {column.render ? column.render(row) : String((row as Record<string, unknown>)[column.key] ?? "—")}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

/* ------------------------------------------------------------------ text */

/** A value with a Copy button (a command, a URL, a key shown once). */
export function CopyBlock({ value, label, multiline }: { value: string; label?: ReactNode; multiline?: boolean }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(value);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {
      /* the text stays selectable */
    }
  };
  return (
    <div className="copy-block">
      <div className="copy-head">
        {label ? <h3>{label}</h3> : <span />}
        <button type="button" onClick={copy} aria-label={copied ? "Copied" : "Copy"}>
          {copied ? <Check size={14} aria-hidden /> : <Copy size={14} aria-hidden />} {copied ? "Copied" : "Copy"}
        </button>
      </div>
      <pre className={cx(multiline && "whitespace-pre-wrap")}>
        <code>{value}</code>
      </pre>
    </div>
  );
}

/** An identifier in mono, shortened, with the whole value as its title. */
export function Mono({ children, short }: { children: string | null | undefined; short?: number }) {
  if (!children) return <span className="muted">—</span>;
  const text = short && children.length > short + 1 ? `${children.slice(0, short)}…` : children;
  return (
    <code className="identifier" title={children}>
      {text}
    </code>
  );
}

export function Kbd({ children }: { children: ReactNode }) {
  return <kbd>{children}</kbd>;
}

/* ------------------------------------------------------------------ forms */

/** A labelled field. A single input/select/textarea child is named by `for`/`id` and described by
 * its help or error; anything else is wrapped by the label. */
export function Field({ label, help, error, required, children }: { label: ReactNode; help?: ReactNode; error?: ReactNode; required?: boolean; children: ReactNode }) {
  const id = useId();
  const describedBy = error ? `${id}-error` : help ? `${id}-help` : undefined;
  const only = Children.count(children) === 1 && isValidElement(children) && typeof children.type === "string" && ["input", "select", "textarea"].includes(children.type);
  const control = only
    ? cloneElement(children as ReactElement<Record<string, unknown>>, {
        id,
        "aria-describedby": describedBy,
        "aria-invalid": error ? true : undefined,
        "aria-required": required || undefined,
        required,
      })
    : children;
  return (
    <div className="field">
      {only ? (
        <label htmlFor={id}>
          {label}
          {required && <span aria-hidden> *</span>}
        </label>
      ) : (
        <span className="field-label">{label}</span>
      )}
      {control}
      {help && !error && (
        <small id={`${id}-help`} className="muted">
          {help}
        </small>
      )}
      {error && (
        <small id={`${id}-error`} className="field-error" role="alert">
          {error}
        </small>
      )}
    </div>
  );
}

/** A stacked form (lake's `form.stack`) that reports the action's error under the field the API's
 * JSON pointer names, or above the actions when no field is at fault. */
export function Form({
  onSubmit,
  children,
  error,
  className,
  id,
}: {
  onSubmit: () => void | Promise<unknown>;
  children: ReactNode;
  error?: unknown;
  className?: string;
  id?: string;
}) {
  return (
    <form
      id={id}
      className={cx("stack", className)}
      onSubmit={(e) => {
        e.preventDefault();
        void onSubmit();
      }}
    >
      {error ? <FormError error={error} /> : null}
      {children}
    </form>
  );
}

/** A refused write, in lake's words: the API's message (a field's own error sits under it). */
export function FormError({ error }: { error: unknown }) {
  if (!error) return null;
  const message = error instanceof Error ? error.message : String(error);
  return (
    <p className="error" role="alert">
      {message}
    </p>
  );
}

/** The message for one field from an action's error, when its JSON pointer names that field. */
export function fieldError(error: unknown, field: string): string | undefined {
  if (error instanceof ApiError && error.path && (error.path === `/${field}` || error.path.startsWith(`/${field}/`))) return error.message;
  return undefined;
}
