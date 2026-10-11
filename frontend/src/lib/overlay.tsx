/** Modal, Drawer and ConfirmButton: the only elevated surfaces. Panels carry no shadow; overlays
 * use lake's dialog shadow so they read as above the page.
 *
 * A Modal with `sheet` is, on a phone, a full-screen sheet with rev-os's mechanics (lake#561;
 * lib/phoneSheet.ts, docs/ui/mobile-web.md): sized to the visual viewport, the page behind locked,
 * inside the safe areas, its body scrolling on its own, the focused field brought above the
 * keyboard, and `SheetActions` pinned to its bottom. On a wide screen it is the same dialog as any
 * other. */
import { createContext, type CSSProperties, type ReactNode, useContext, useEffect, useId, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { X } from "lucide-react";
import { Button, cx } from "./ui";
import { backdropMayClose } from "./clicks";
import { useIsPhone, usePhoneSheet } from "./phoneSheet";

/** Where a phone sheet pins its actions; absent everywhere else. */
const SheetFooter = createContext<HTMLElement | null>(null);

/** A form's actions (Save, Create…). In a phone sheet they are pinned to the sheet's bottom, so they
 * sit just above the keyboard whatever is scrolled; anywhere else they stay where they are written. */
export function SheetActions({ children, className }: { children: ReactNode; className?: string }) {
  const slot = useContext(SheetFooter);
  const row = <div className={className}>{children}</div>;
  return slot ? createPortal(row, slot) : row;
}

/** The backdrop closes what it covers on a deliberate press only: one that starts and ends on it,
 * and not the opener's second click or a double-click landing on it just after the overlay
 * appeared (lib/clicks.ts). Escape always closes. Focus moves into the overlay and returns to the
 * opener on close. */
export function Overlay({ onClose, children, align = "center", label }: { onClose: () => void; children: ReactNode; align?: "center" | "right"; label?: string }) {
  const openedAt = useRef(Number.POSITIVE_INFINITY);
  const pressedOnBackdrop = useRef(false);
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    openedAt.current = performance.now();
    const opener = document.activeElement as HTMLElement | null;
    const first = box.current?.querySelector<HTMLElement>("[autofocus], input, select, textarea, button, a[href]");
    first?.focus();
    return () => {
      opener?.focus?.();
    };
  }, []);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    const prev = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    return () => {
      window.removeEventListener("keydown", onKey);
      document.body.style.overflow = prev;
    };
  }, [onClose]);
  return createPortal(
    <div
      ref={box}
      className={align === "center" ? "overlay overlay-center" : "overlay overlay-right"}
      aria-label={label}
      onMouseDown={(e) => {
        pressedOnBackdrop.current = e.target === e.currentTarget;
      }}
      onClick={(e) => {
        const press = { startedOnBackdrop: pressedOnBackdrop.current, endedOnBackdrop: e.target === e.currentTarget, detail: e.detail };
        pressedOnBackdrop.current = false;
        if (backdropMayClose(press, openedAt.current, performance.now())) onClose();
      }}
    >
      {children}
    </div>,
    document.body,
  );
}

export function Modal({
  title,
  subtitle,
  onClose,
  children,
  footer,
  width = 560,
  sheet = false,
}: {
  title: ReactNode;
  subtitle?: ReactNode;
  onClose: () => void;
  children: ReactNode;
  footer?: ReactNode;
  width?: number;
  /** On a phone: a full-screen sheet, its actions above the keyboard. */
  sheet?: boolean;
}) {
  const phone = useIsPhone();
  // One tree either way: turning a phone sideways (past the breakpoint) restyles the dialog and
  // never remounts what is in it, so a half-written form survives.
  const asSheet = sheet && phone;
  const body = useRef<HTMLDivElement>(null);
  const [slot, setSlot] = useState<HTMLDivElement | null>(null);
  const keyboard = usePhoneSheet(asSheet, body);
  // The home indicator's strip matters only while the keyboard is down.
  const style = (asSheet ? { "--sheet-safe-bottom": keyboard ? "0px" : "env(safe-area-inset-bottom)" } : { maxWidth: width }) as CSSProperties;
  return (
    <Overlay onClose={onClose}>
      <div role="dialog" aria-modal className={cx("dialog", asSheet && "dialog-sheet")} data-sheet={asSheet || undefined} style={style}>
        <div className="dialog-head">
          <div className="min-w-0 flex-1">
            <h2 className="dialog-title">{title}</h2>
            {subtitle && <p className="muted m-0">{subtitle}</p>}
          </div>
          <button type="button" className="icon-button dialog-close" onClick={onClose} aria-label="Close">
            <X size={asSheet ? 20 : 16} aria-hidden />
          </button>
        </div>
        <div ref={body} data-modal-body className="dialog-body">
          <SheetFooter.Provider value={asSheet ? slot : null}>{children}</SheetFooter.Provider>
        </div>
        {/* A phone sheet's pinned actions (SheetActions); hidden while empty. */}
        <div ref={setSlot} className={cx("empty:hidden", asSheet && "sheet-actions")} />
        {footer && <div className="dialog-foot">{footer}</div>}
      </div>
    </Overlay>
  );
}

export function Drawer({
  title,
  subtitle,
  onClose,
  children,
  footer,
  width = 520,
}: {
  title: ReactNode;
  subtitle?: ReactNode;
  onClose: () => void;
  children: ReactNode;
  footer?: ReactNode;
  width?: number;
}) {
  return (
    <Overlay onClose={onClose} align="right">
      <aside role="dialog" aria-modal className="drawer" style={{ maxWidth: width }}>
        <div className="dialog-head">
          <div className="min-w-0 flex-1">
            <h2 className="dialog-title">{title}</h2>
            {subtitle && <p className="muted m-0">{subtitle}</p>}
          </div>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close">
            <X size={16} aria-hidden />
          </button>
        </div>
        <div className="dialog-body flex-1">{children}</div>
        {footer && <div className="dialog-foot">{footer}</div>}
      </aside>
    </Overlay>
  );
}

/** A consequential action that asks once, in words. The dialog names what happens ("Delete the
 * repository?"), never "Are you sure?". An error from the action stays in the dialog. With `typed`,
 * the action is irreversible: Confirm stays disabled until that exact text is typed. */
export function ConfirmButton({
  children,
  confirm,
  onConfirm,
  variant = "secondary",
  confirmLabel = "Confirm",
  title,
  disabled,
  typed,
  danger,
}: {
  children: ReactNode;
  confirm: ReactNode;
  onConfirm: () => void | Promise<unknown>;
  variant?: "primary" | "secondary" | "danger" | "ghost";
  confirmLabel?: string;
  title?: ReactNode;
  disabled?: boolean;
  /** Text the person must type before Confirm is enabled (a name, for an action that cannot be undone). */
  typed?: string;
  /** The Confirm button reads as destructive even when the trigger is quiet (a row's ghost button). */
  danger?: boolean;
}) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [entered, setEntered] = useState("");
  const typedId = useId();
  return (
    <>
      <Button
        variant={variant}
        onClick={() => {
          setEntered("");
          setError(null);
          setOpen(true);
        }}
        disabled={disabled}
      >
        {children}
      </Button>
      {open && (
        <Modal
          title={title ?? `${confirmLabel}?`}
          onClose={() => setOpen(false)}
          width={460}
          footer={
            <>
              <Button onClick={() => setOpen(false)}>Cancel</Button>
              <Button
                variant={variant === "danger" || danger ? "danger" : "primary"}
                loading={busy}
                disabled={typed !== undefined && entered !== typed}
                onClick={async () => {
                  setBusy(true);
                  setError(null);
                  try {
                    await onConfirm();
                    setOpen(false);
                  } catch (e) {
                    setError((e as Error).message);
                  } finally {
                    setBusy(false);
                  }
                }}
              >
                {confirmLabel}
              </Button>
            </>
          }
        >
          <div className="muted">{confirm}</div>
          {typed !== undefined && (
            <div className="field mt-3 mb-0">
              <label htmlFor={typedId}>
                Type <code>{typed}</code> to confirm
              </label>
              <input id={typedId} value={entered} onChange={(e) => setEntered(e.target.value)} autoComplete="off" spellCheck={false} />
            </div>
          )}
          {error && (
            <div className="error mt-3" role="alert">
              {error}
            </div>
          )}
        </Modal>
      )}
    </>
  );
}
