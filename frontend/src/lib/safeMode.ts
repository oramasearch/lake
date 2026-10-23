/** A database's safe mode (lake#906, docs/ui/safe-mode.md): Off, Warn Writes or Warn All, kept in
 * the workspace per database or connection (`get_safe_mode`, `set_safe_mode`). A call the level
 * warns on is refused with `safe_mode_warning`, whose details carry the exact statements and a
 * confirmation; the same call with `safe_mode_confirmation` runs them. Pure, so
 * `tests/safe-mode.test.ts` runs it under Node. */

export type SafeMode = "off" | "warn_writes" | "warn_all";

/** The levels in the order the menu lists them. Warn Writes is the workspace's default. */
export const SAFE_MODES: { mode: SafeMode; label: string; help: string }[] = [
  { mode: "off", label: "Off", help: "Run every query without asking." },
  { mode: "warn_writes", label: "Warn Writes", help: "Ask before a query that changes data." },
  { mode: "warn_all", label: "Warn All", help: "Ask before every query." },
];

export const DEFAULT_SAFE_MODE: SafeMode = "warn_writes";

export function isSafeMode(value: unknown): value is SafeMode {
  return value === "off" || value === "warn_writes" || value === "warn_all";
}

/** The level's words: "Warn Writes", never `warn_writes`. */
export function safeModeLabel(mode: unknown): string {
  return SAFE_MODES.find((m) => m.mode === mode)?.label ?? "Unknown";
}

/** What a safe mode applies to: one of the workspace's databases or a saved connection. */
export type SafeModeTarget = { database: string } | { connection: string };

export function targetInput(target: SafeModeTarget): Record<string, string> {
  return "connection" in target ? { connection: target.connection } : { database: target.database };
}

export function targetName(target: SafeModeTarget): string {
  return "connection" in target ? target.connection : target.database;
}

export type SafeModeWarning = {
  target: SafeModeTarget;
  mode: SafeMode | null;
  statements: string[];
  confirmation: string;
  message: string;
};

/** The warning an error carries, or null when it is any other error. */
export function safeModeWarning(error: unknown): SafeModeWarning | null {
  if (!error || typeof error !== "object") return null;
  const failure = error as { code?: unknown; details?: unknown; message?: unknown };
  if (failure.code !== "safe_mode_warning") return null;
  const details = (failure.details && typeof failure.details === "object" ? failure.details : {}) as Record<string, unknown>;
  const confirmation = typeof details.confirmation === "string" ? details.confirmation : "";
  if (!confirmation) return null;
  const connection = typeof details.connection === "string" ? details.connection : "";
  const database = typeof details.database === "string" ? details.database : "";
  const statements = Array.isArray(details.statements) ? details.statements.filter((s): s is string => typeof s === "string") : [];
  return {
    target: connection ? { connection } : { database },
    mode: isSafeMode(details.mode) ? details.mode : null,
    statements,
    confirmation,
    message: typeof failure.message === "string" ? failure.message : "",
  };
}

/** A capability's input with the warning's confirmation added, or unchanged without one. */
export function withConfirmation<T extends Record<string, unknown>>(input: T, confirmation: string | undefined | null): T & { safe_mode_confirmation?: string } {
  return confirmation ? { ...input, safe_mode_confirmation: confirmation } : input;
}

/** The database a query editor's safe mode applies to: the Database field, or else the database
 * that qualifies the statement's first table (`FROM datasets.accounts` → `datasets`). Empty when
 * neither names one. */
export function queryDatabase(database: string, statement: string): string {
  const named = database.trim();
  if (named) return named;
  const from = /\b(?:from|join|into|update|table)\s+"?([A-Za-z_][A-Za-z0-9_]*)"?\s*\.\s*"?[A-Za-z_][A-Za-z0-9_]*/i.exec(statement);
  return from ? from[1] : "";
}

/** The dialog's heading line: which level asked, about what. */
export function warningSummary(warning: SafeModeWarning): string {
  const where = "connection" in warning.target ? `the connection ${warning.target.connection}` : `the database ${warning.target.database}`;
  const level = warning.mode ? safeModeLabel(warning.mode) : "Safe mode";
  const count = warning.statements.length;
  if (count === 0) return `${level} is on for ${where}. Confirm to run the call.`;
  return `${level} is on for ${where}. Check ${count === 1 ? "the query" : `the ${count} queries`} before ${count === 1 ? "it runs" : "they run"}.`;
}
