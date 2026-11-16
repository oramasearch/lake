// SPDX-License-Identifier: MIT

/** Formatting — numbers are mono and tabular in the UI; these produce the text. */

export function money(value: unknown, opts: { compact?: boolean; cents?: boolean } = {}): string {
  const n = toNumber(value);
  if (n === null) return "—";
  if (opts.compact && Math.abs(n) >= 1000) {
    const units: [number, string][] = [
      [1e9, "B"],
      [1e6, "M"],
      [1e3, "K"],
    ];
    for (const [size, suffix] of units) {
      if (Math.abs(n) >= size) {
        const v = n / size;
        return `$${v >= 100 ? v.toFixed(0) : v.toFixed(1).replace(/\.0$/, "")}${suffix}`;
      }
    }
  }
  return n.toLocaleString("en-US", {
    style: "currency",
    currency: "USD",
    maximumFractionDigits: opts.cents ? 2 : 0,
    minimumFractionDigits: opts.cents ? 2 : 0,
  });
}

export function num(value: unknown, digits = 0): string {
  const n = toNumber(value);
  if (n === null) return "—";
  return n.toLocaleString("en-US", { maximumFractionDigits: digits, minimumFractionDigits: digits });
}

/** A rate stored as a fraction (0.034) or a percentage (3.4) → "3.4%". */
export function pct(value: unknown, digits = 1, isFraction = true): string {
  const n = toNumber(value);
  if (n === null) return "—";
  return `${(isFraction ? n * 100 : n).toFixed(digits).replace(/\.0+$/, "")}%`;
}

export function toNumber(value: unknown): number | null {
  if (value === null || value === undefined || value === "") return null;
  const n = typeof value === "number" ? value : Number(value);
  return Number.isFinite(n) ? n : null;
}

/** "2026-10-03" — a calendar date with no time of day (a date field). */
const DATE_ONLY = /^\d{4}-\d{2}-\d{2}$/;

export function isDateOnly(value: unknown): value is string {
  return typeof value === "string" && DATE_ONLY.test(value);
}

/**
 * Parse an API value into a Date.
 *
 * A date-only value ("2026-10-03") is a calendar date, not an instant: it is
 * read as local midnight of that day, so it shows as the stored date in every
 * time zone. `new Date("2026-10-03")` reads it as UTC midnight and shows the
 * day before anywhere west of Greenwich. Instants (ISO with an offset)
 * and epoch milliseconds are read as they are.
 */
export function parseDate(value: unknown): Date | null {
  if (value === null || value === undefined || value === "") return null;
  if (value instanceof Date) return Number.isNaN(value.getTime()) ? null : value;
  if (typeof value === "number") return Number.isFinite(value) ? new Date(value) : null;
  const s = String(value);
  if (DATE_ONLY.test(s)) {
    const [y, m, d] = s.split("-").map(Number);
    return new Date(y, m - 1, d);
  }
  const d = new Date(s);
  return Number.isNaN(d.getTime()) ? null : d;
}

const pad = (n: number) => String(n).padStart(2, "0");

/** A Date → "2026-10-03" in the user's own time zone (never UTC). */
export function isoDate(d: Date): string {
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

/** Today's calendar date where the user is — the default for a date field. */
export function todayISO(now: Date = new Date()): string {
  return isoDate(now);
}

/** A calendar date `days` from today where the user is. */
export function addDaysISO(days: number, from: Date = new Date()): string {
  const d = new Date(from.getFullYear(), from.getMonth(), from.getDate() + days);
  return isoDate(d);
}

/** An instant → the "YYYY-MM-DDTHH:mm" a datetime-local input shows, in local time. */
export function toLocalInput(value: unknown): string {
  const d = parseDate(value);
  if (!d) return "";
  return `${isoDate(d)}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** An instant → ISO 8601 with the user's own UTC offset ("2026-10-03T17:00:00-07:00"). */
export function isoWithOffset(d: Date): string {
  const off = -d.getTimezoneOffset();
  const sign = off >= 0 ? "+" : "-";
  const a = Math.abs(off);
  return `${isoDate(d)}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}${sign}${pad(Math.floor(a / 60))}:${pad(a % 60)}`;
}

/**
 * What a datetime-local input holds ("2026-10-03T17:00", the user's wall
 * clock) → ISO with the user's offset, so the server stores the instant the
 * user meant instead of reading the bare value in its own zone.
 * Empty → null.
 */
export function fromLocalInput(value: string | null | undefined): string | null {
  if (!value) return null;
  const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(value);
  if (!m) return value;
  const [, y, mo, d, h, mi, sec] = m;
  return isoWithOffset(new Date(Number(y), Number(mo) - 1, Number(d), Number(h), Number(mi), Number(sec ?? 0)));
}

/**
 * A calendar date picked for a date-and-time field (a due date with no time):
 * 17:00 — the end of the working day — in the user's zone, sent with its
 * offset, so it reads as the same calendar date in every US time zone.
 */
export function dueFromDate(value: string | null | undefined): string | null {
  if (!value) return null;
  if (!isDateOnly(value)) return fromLocalInput(value);
  return fromLocalInput(`${value}T17:00`);
}

/** An instant → the "YYYY-MM-DD" a date input shows, in local time. */
export function toDateInput(value: unknown): string {
  const d = parseDate(value);
  return d ? isoDate(d) : "";
}

export function date(value: unknown, opts: Intl.DateTimeFormatOptions = {}): string {
  if (value === null || value === undefined || value === "") return "—";
  const d = parseDate(value);
  if (!d) return String(value);
  return d.toLocaleDateString("en-US", { month: "short", day: "numeric", year: "numeric", ...opts });
}

/** "3 October 2026" — the long form a client-facing document uses. */
export function longDate(value: unknown): string {
  const d = parseDate(value);
  if (!d) return "";
  return d.toLocaleDateString("en-GB", { day: "numeric", month: "long", year: "numeric" });
}

export function dateTime(value: unknown): string {
  if (value === null || value === undefined || value === "") return "—";
  if (isDateOnly(value)) return date(value);
  const d = parseDate(value);
  if (!d) return String(value);
  return d.toLocaleString("en-US", {
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
  });
}

function instant(value: unknown): number | null {
  const d = parseDate(value);
  return d ? d.getTime() : null;
}

function span(s: number): string {
  if (s < 3600) return `${Math.max(1, Math.round(s / 60))}m`;
  if (s < 86400) return `${Math.round(s / 3600)}h`;
  if (s < 86400 * 14) return `${Math.round(s / 86400)}d`;
  return `${Math.round(s / (86400 * 7))}w`;
}

/** "3h", "2d", "5w" — the age of an exception or a wait. No value → "—". */
export function age(value: unknown, now = Date.now()): string {
  const t = instant(value);
  if (t === null) return "—";
  return span(Math.max(0, (now - t) / 1000));
}

/**
 * Relative: "in 6h", "in 3d" / "2h ago"; a future more than two weeks out
 * reads as its date ("Oct 29"). No value → `empty` ("—" by default; pass
 * "never" where nothing has happened yet) — never "— ago" or "in —".
 */
export function relative(value: unknown, now = Date.now(), empty = "—"): string {
  const t = instant(value);
  if (t === null) return empty;
  if (t > now) {
    const s = (t - now) / 1000;
    if (s >= 86400 * 14) {
      const sameYear = new Date(t).getFullYear() === new Date(now).getFullYear();
      return date(value, sameYear ? { year: undefined } : {});
    }
    return `in ${span(s)}`;
  }
  return `${span((now - t) / 1000)} ago`;
}

/** A stored period key as words: "last_30d" → "last 30 days", "2026-10" → "Oct 2026". */
export function periodLabel(value: unknown): string {
  if (value === null || value === undefined || value === "") return "—";
  const s = String(value);
  const last = /^last_(\d+)([dwm])$/.exec(s);
  if (last) {
    const n = Number(last[1]);
    const unit = { d: "day", w: "week", m: "month" }[last[2] as "d" | "w" | "m"];
    return `last ${n} ${unit}${n === 1 ? "" : "s"}`;
  }
  const month = /^(\d{4})-(\d{2})$/.exec(s);
  if (month) return new Date(Number(month[1]), Number(month[2]) - 1, 1).toLocaleDateString("en-US", { month: "short", year: "numeric" });
  return s.replace(/_/g, " ");
}

export function initials(name: string | undefined | null): string {
  if (!name) return "?";
  const parts = name.trim().split(/\s+/);
  return ((parts[0]?.[0] ?? "") + (parts.length > 1 ? parts[parts.length - 1][0] : "")).toUpperCase();
}

export function titleCase(value: string | null | undefined): string {
  if (!value) return "—";
  return value
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/\b\w/g, (c) => c.toUpperCase());
}

export function plural(n: number, one: string, many?: string): string {
  return `${num(n)} ${n === 1 ? one : (many ?? one + "s")}`;
}


/** A byte count in binary units: "1.4 MiB". */
export function bytes(value: unknown): string {
  const n = toNumber(value);
  if (n === null) return "—";
  const units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];
  let size = n;
  let unit = 0;
  while (Math.abs(size) >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${size} B` : `${size.toFixed(size >= 100 ? 0 : 1).replace(/\.0$/, "")} ${units[unit]}`;
}

/** A long identifier shortened for display, the whole value kept for copying: "3f9a1c2e…". */
export function shortId(value: string | null | undefined, length = 8): string {
  if (!value) return "—";
  return value.length > length + 1 ? `${value.slice(0, length)}…` : value;
}
