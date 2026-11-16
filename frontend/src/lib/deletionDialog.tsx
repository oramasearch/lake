// SPDX-License-Identifier: MIT

/** The one confirmation every delete and revoke uses (lake#645, docs/ui/deletion.md). It asks the
 * capability with `dry_run: true` first (a read over POST, which changes and refreshes nothing),
 * lists what would go from that answer, and keeps the action disabled until the person types the
 * confirmation text. Only then does it call the capability for real; `act` refreshes every read
 * on screen. A refusal stays in the dialog, and the plan is never asked again while the action
 * is under way. */
import { type ReactNode, useId, useState } from "react";
import { useQuery } from "@tanstack/react-query";
import { Trash2 } from "lucide-react";
import { act, postRead, type Row } from "@/lib/api";
import type { DeletionLine } from "@/lib/deletion";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, ErrorBlock, Skeleton } from "@/lib/ui";

export type DeletionSpec = {
  /** The capability, as registered: `delete_dataset`, `revoke_grant`, … */
  capability: string;
  /** Its input, without `dry_run` and `confirm`. */
  input: Row;
  /** The workspace prefix to call under: undefined is the page's, null none. */
  workspace?: string | null;
  /** The dialog's title: "Delete Dataset orders". */
  title: ReactNode;
  /** The danger button: "Delete dataset", "Revoke invitation". */
  confirmLabel: string;
  /** What the person types, from the plan: a name, or a fixed word. */
  typed: (plan: Row) => string;
  /** Sends the typed text as `confirm` (the capabilities that check it: organizations, mounts). */
  sendConfirm?: boolean;
  /** The lines the dialog lists. */
  lines: (plan: Row) => DeletionLine[];
  /** The sentence above the list. */
  intro?: ReactNode;
  /** Why the action cannot go ahead, from the plan; the button stays disabled while it says so. */
  blocked?: (plan: Row) => ReactNode | null;
  /** More below the list, from the plan (names, links). */
  details?: (plan: Row) => ReactNode;
  /** The toast after it is done. */
  done: string;
  onDone?: (answer: Row) => void;
  onClose: () => void;
};

export function DeletionDialog(spec: DeletionSpec) {
  const toast = useToast();
  const typedId = useId();
  const [entered, setEntered] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const options = spec.workspace === undefined ? {} : { workspace: spec.workspace };
  const plan = useQuery<Row>({
    queryKey: ["deletion-plan", spec.capability, spec.input, spec.workspace ?? ""],
    queryFn: ({ signal }) => postRead<Row>(spec.capability, { ...spec.input, dry_run: true }, { ...options, signal }),
    retry: false,
    enabled: !busy,
    staleTime: 0,
    gcTime: 0,
  });
  const value = plan.data;
  const typed = value ? spec.typed(value) : "";
  const blocked = value && spec.blocked ? spec.blocked(value) : null;

  const submit = async () => {
    if (!value || blocked || entered !== typed) return;
    setBusy(true);
    setError(null);
    try {
      const answer = await act<Row>(spec.capability, { ...spec.input, ...(spec.sendConfirm ? { confirm: typed } : {}) }, options);
      toast(spec.done);
      spec.onDone?.(answer);
      spec.onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  let body: ReactNode;
  if (plan.isError) body = <ErrorBlock error={plan.error} what="what this removes" />;
  else if (!value) body = <Skeleton />;
  else {
    body = (
      <>
        {spec.intro && <p className="mt-0 text-[13px] text-ink-2">{spec.intro}</p>}
        <ul className="m-0 list-none divide-y divide-hairline rounded-[6px] border border-hairline px-3" aria-label="What goes">
          {spec.lines(value).map((entry, index) => (
            <li key={index} className="flex items-start gap-3 py-2">
              <span className="min-w-10 shrink-0 text-right font-mono text-[15px] font-semibold tabular-nums text-bad">{entry.count === null ? "·" : entry.count.toLocaleString("en-US")}</span>
              <span className="min-w-0 text-[13px]">{entry.text}</span>
            </li>
          ))}
        </ul>
        {spec.details?.(value)}
        {blocked ? (
          <div className="notice notice-warning mt-3" role="status">
            {blocked}
          </div>
        ) : (
          <form
            className="field mt-3 mb-0"
            onSubmit={(event) => {
              event.preventDefault();
              void submit();
            }}
          >
            <label htmlFor={typedId}>
              Type <code>{typed}</code> to confirm
            </label>
            <input id={typedId} name="confirm" value={entered} onChange={(e) => setEntered(e.target.value)} autoComplete="off" spellCheck={false} />
          </form>
        )}
        {error ? (
          <p className="error mt-3" role="alert">
            {(error as Error).message}
          </p>
        ) : null}
      </>
    );
  }

  return (
    <Modal
      title={spec.title}
      onClose={spec.onClose}
      width={540}
      footer={
        <>
          <Button onClick={spec.onClose}>Cancel</Button>
          <Button variant="danger" loading={busy} disabled={!value || !!blocked || entered !== typed} onClick={() => void submit()}>
            <Trash2 size={14} aria-hidden /> {spec.confirmLabel}
          </Button>
        </>
      }
    >
      {body}
    </Modal>
  );
}

/** A trigger that opens the dialog. `variant` styles the trigger; the confirm is always danger.
 * `hint` is the trigger's tooltip (why it is disabled, say). */
export function DeletionButton({
  children,
  variant = "secondary",
  disabled,
  hint,
  ...spec
}: Omit<DeletionSpec, "onClose"> & { children: ReactNode; variant?: "secondary" | "danger" | "ghost"; disabled?: boolean; hint?: string; onClose?: () => void }) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <Button variant={variant} disabled={disabled} title={hint} onClick={() => setOpen(true)}>
        {children}
      </Button>
      {open && (
        <DeletionDialog
          {...spec}
          onClose={() => {
            setOpen(false);
            spec.onClose?.();
          }}
        />
      )}
    </>
  );
}
