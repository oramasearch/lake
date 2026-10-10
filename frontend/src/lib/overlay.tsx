/** Modal, Drawer and ConfirmButton: the only elevated surfaces. Panels carry no shadow; overlays
 * use lake's dialog shadow so they read as above the page. */
import { type ReactNode, useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { X } from "lucide-react";
import { Button } from "./ui";
import { backdropMayClose } from "./clicks";

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
}: {
  title: ReactNode;
  subtitle?: ReactNode;
  onClose: () => void;
  children: ReactNode;
  footer?: ReactNode;
  width?: number;
}) {
  return (
    <Overlay onClose={onClose}>
      <div role="dialog" aria-modal className="dialog" style={{ maxWidth: width }}>
        <div className="dialog-head">
          <div className="min-w-0 flex-1">
            <h2 className="dialog-title">{title}</h2>
            {subtitle && <p className="muted m-0">{subtitle}</p>}
          </div>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close">
            <X size={16} aria-hidden />
          </button>
        </div>
        <div className="dialog-body">{children}</div>
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
 * repository?"), never "Are you sure?". An error from the action stays in the dialog. */
export function ConfirmButton({
  children,
  confirm,
  onConfirm,
  variant = "secondary",
  confirmLabel = "Confirm",
  title,
  disabled,
}: {
  children: ReactNode;
  confirm: ReactNode;
  onConfirm: () => void | Promise<unknown>;
  variant?: "primary" | "secondary" | "danger" | "ghost";
  confirmLabel?: string;
  title?: ReactNode;
  disabled?: boolean;
}) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <>
      <Button variant={variant} onClick={() => setOpen(true)} disabled={disabled}>
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
                variant={variant === "danger" ? "danger" : "primary"}
                loading={busy}
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
