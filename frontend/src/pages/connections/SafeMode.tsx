/** A database's safe mode in the web app (lake#906, docs/ui/safe-mode.md): the control that shows
 * and changes the level (a lock and the level; its menu lists Off, Warn Writes and Warn All with a
 * check on the current one), and the dialog a `safe_mode_warning` opens, which shows the exact
 * statements and the database with Run and Cancel. Run repeats the same call with the warning's
 * confirmation. The level lives in the workspace; Lake keeps no copy. */
import { type ReactNode, useEffect, useRef, useState } from "react";
import { Check, ChevronDown, Lock, LockOpen } from "lucide-react";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, cx } from "@/lib/ui";
import {
  defaultSafeMode,
  SAFE_MODES,
  type SafeMode,
  type SafeModeTarget,
  type SafeModeWarning,
  isSafeMode,
  safeModeLabel,
  safeModeWarning,
  targetInput,
  targetName,
  warningSummary,
} from "@/lib/safeMode";
import { useWorkspace, useWq, wa } from "@/pages/train/workspace";

/** The level of one database or connection, and the menu that changes it. `status` is the query
 * editor's status-bar item; `field` sits in a card beside its label. */
export function SafeModeControl({ target, variant = "field" }: { target: SafeModeTarget | null; variant?: "status" | "field" }) {
  const scope = useWorkspace();
  const toast = useToast();
  const name = target ? targetName(target) : "";
  const level = useWq<{ mode?: string }>(target && name ? "get_safe_mode" : null, target ? targetInput(target) : {}, { retry: false });
  const [open, setOpen] = useState(false);
  const [saving, setSaving] = useState<SafeMode | null>(null);
  const [error, setError] = useState<string | null>(null);
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", key);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", key);
    };
  }, [open]);

  const mode: SafeMode | null = saving ?? (isSafeMode(level.data?.mode) ? level.data.mode : level.data && target ? defaultSafeMode(target) : null);
  const unavailable = !target || !name || level.isError;
  const why = !target || !name ? "Name a database to see its safe mode." : level.isError ? (level.error as Error).message : "";
  const label = mode ? safeModeLabel(mode) : unavailable ? "Safe mode" : "…";
  const Icon = mode === "off" ? LockOpen : Lock;
  const choose = async (next: SafeMode) => {
    if (!target || next === mode) {
      setOpen(false);
      return;
    }
    setOpen(false);
    setSaving(next);
    setError(null);
    try {
      await wa(scope, "set_safe_mode", { ...targetInput(target), mode: next });
      toast(`Safe mode for ${name}: ${safeModeLabel(next)}`);
      await level.refetch();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSaving(null);
    }
  };
  const trigger =
    variant === "status"
      ? "inline-flex h-7 items-center gap-1.5 rounded-[5px] px-2 text-[12px] text-ink-2 hover:bg-raised hover:text-ink disabled:text-ink-disabled disabled:hover:bg-transparent"
      : "button inline-flex items-center gap-1.5";
  return (
    <div ref={box} className={cx("relative", variant === "field" && "inline-block")}>
      <button
        type="button"
        className={trigger}
        onClick={() => setOpen((o) => !o)}
        disabled={unavailable}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label={`Safe mode${name ? ` for ${name}` : ""}: ${label}`}
        title={why || `Safe mode for ${name}: ${label}`}
      >
        <Icon size={13} aria-hidden className={cx(mode && mode !== "off" ? "text-warn" : "text-ink-3")} />
        <span>{label}</span>
        {variant === "field" && <ChevronDown size={13} aria-hidden className="text-ink-3" />}
      </button>
      {open && (
        <div
          role="menu"
          aria-label="Safe mode"
          className={cx(
            "absolute z-50 w-[248px] overflow-hidden rounded-[6px] border border-hairline bg-overlay py-1 shadow-popover",
            variant === "status" ? "bottom-full left-0 mb-1" : "top-full left-0 mt-1",
          )}
        >
          <div className="px-3 py-1.5 text-[11.5px] font-semibold tracking-wide text-ink-3 uppercase">Safe mode</div>
          {SAFE_MODES.map((option) => (
            <button
              key={option.mode}
              type="button"
              role="menuitemradio"
              aria-checked={option.mode === mode}
              onClick={() => void choose(option.mode)}
              className="flex w-full items-start gap-2 px-3 py-1.5 text-left text-[13px] text-ink hover:bg-raised"
            >
              <span className="mt-0.5 inline-flex w-3.5 shrink-0 justify-center">{option.mode === mode && <Check size={13} aria-hidden />}</span>
              <span className="min-w-0">
                <span className="block">{option.label}</span>
                <span className="block text-[12px] text-ink-3">{option.help}</span>
              </span>
            </button>
          ))}
        </div>
      )}
      {error && (
        <div role="alert" className="mt-1 text-[12px] text-bad">
          {error}
        </div>
      )}
    </div>
  );
}

/** The warning's dialog: the database or connection, the level, every statement as it will run,
 * and Run or Cancel. It is a confirmation by the person running the query, not an approval by
 * someone else. */
export function SafeModeDialog({ warning, onRun, onCancel, busy }: { warning: SafeModeWarning; onRun: () => void; onCancel: () => void; busy?: boolean }) {
  const where = "connection" in warning.target ? "Connection" : "Database";
  return (
    <Modal
      title="Run This Query?"
      subtitle={warningSummary(warning)}
      onClose={onCancel}
      width={640}
      footer={
        <>
          <Button onClick={onCancel}>Cancel</Button>
          <Button variant="primary" onClick={onRun} loading={busy}>
            Run
          </Button>
        </>
      }
    >
      <dl className="m-0 grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-[12.5px]">
        <dt className="text-ink-3">{where}</dt>
        <dd className="m-0 min-w-0 truncate font-mono">{targetName(warning.target) || "—"}</dd>
        <dt className="text-ink-3">Safe mode</dt>
        <dd className="m-0">{warning.mode ? safeModeLabel(warning.mode) : "—"}</dd>
      </dl>
      <div className="mt-3 space-y-2" aria-label="Queries that will run">
        {warning.statements.length === 0 ? (
          <p className="muted m-0 text-[12.5px]">{warning.message || "The workspace did not say which statements it would run."}</p>
        ) : (
          warning.statements.map((statement, i) => (
            <pre key={i} className="m-0 max-h-60 overflow-auto rounded-[5px] border border-hairline bg-raised px-3 py-2 font-mono text-[12.5px] whitespace-pre-wrap text-ink">
              {statement}
            </pre>
          ))
        )}
      </div>
    </Modal>
  );
}

/** Safe mode for a read the page keeps as a query (the query editor, a connection's tables). The
 * page passes the read's error and the key of what it sent; a warning opens the dialog, Run gives
 * back the confirmation to send with that same read, and Cancel leaves a notice to reopen it. */
export function useSafeModeRead(error: unknown, key: string): { confirmation: string | undefined; dialog: ReactNode; notice: ReactNode } {
  const [confirmed, setConfirmed] = useState<{ key: string; confirmation: string } | null>(null);
  const [dismissed, setDismissed] = useState<string | null>(null);
  const warning = safeModeWarning(error);
  const open = !!warning && dismissed !== warning.confirmation;
  const confirmation = confirmed && confirmed.key === key ? confirmed.confirmation : undefined;
  const dialog =
    warning && open ? (
      <SafeModeDialog
        warning={warning}
        onRun={() => {
          setConfirmed({ key, confirmation: warning.confirmation });
          setDismissed(null);
        }}
        onCancel={() => setDismissed(warning.confirmation)}
      />
    ) : null;
  const notice =
    warning && !open ? (
      <Banner tone="quiet" title="Not run" action={<Button onClick={() => setDismissed(null)}>Review</Button>}>
        {safeModeLabel(warning.mode)} is on for {targetName(warning.target)}, and the query was cancelled.
      </Banner>
    ) : null;
  return { confirmation, dialog, notice };
}

/** The thrown value when a person cancels a safe mode warning: the form shows nothing for it. */
export class SafeModeCancelled extends Error {
  constructor() {
    super("Cancelled.");
  }
}

/** Safe mode for an action (pulling a table into a dataset): `run(call)` calls it; on a warning it
 * shows the dialog and, on Run, calls it again with the confirmation; on Cancel it throws
 * SafeModeCancelled. Render `dialog` once in the component. */
export function useSafeModeAction(): { run: <T>(call: (confirmation?: string) => Promise<T>) => Promise<T>; dialog: ReactNode } {
  const [pending, setPending] = useState<{ warning: SafeModeWarning; resolve: (run: boolean) => void } | null>(null);
  const ask = (warning: SafeModeWarning) =>
    new Promise<boolean>((resolve) => {
      setPending({ warning, resolve });
    });
  const run = async <T,>(call: (confirmation?: string) => Promise<T>): Promise<T> => {
    let confirmation: string | undefined;
    // A confirmation answers one warning; should the workspace warn again (the statements
    // changed), the person sees the new ones.
    for (;;) {
      try {
        return await call(confirmation);
      } catch (e) {
        const warning = safeModeWarning(e);
        if (!warning) throw e;
        if (!(await ask(warning))) throw new SafeModeCancelled();
        confirmation = warning.confirmation;
      }
    }
  };
  const settle = (answer: boolean) => {
    pending?.resolve(answer);
    setPending(null);
  };
  const dialog = pending ? <SafeModeDialog warning={pending.warning} onRun={() => settle(true)} onCancel={() => settle(false)} /> : null;
  return { run, dialog };
}
