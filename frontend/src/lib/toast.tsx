// SPDX-License-Identifier: MIT

/** Toasts: a short confirmation after a write ("Repository created"), or a failure that has no
 * field to sit under. Lake's .toast style; several stack from the bottom right. */
import { createContext, type ReactNode, useCallback, useContext, useState } from "react";
import { createPortal } from "react-dom";
import { cx } from "./ui";

type Toast = { id: number; tone: "ok" | "bad" | "info"; text: ReactNode };
type ToastFn = (text: ReactNode, tone?: Toast["tone"]) => void;

const Ctx = createContext<ToastFn>(() => {});

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const push = useCallback<ToastFn>((text, tone = "ok") => {
    const id = Date.now() + Math.random();
    setToasts((t) => [...t.slice(-3), { id, tone, text }]);
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), tone === "bad" ? 6000 : 3200);
  }, []);
  return (
    <Ctx.Provider value={push}>
      {children}
      {createPortal(
        <div className="toast-stack" aria-live="polite">
          {toasts.map((t) => (
            <div key={t.id} role={t.tone === "bad" ? "alert" : "status"} className={cx("toast", t.tone === "ok" && "toast-ok", t.tone === "bad" && "toast-bad")}>
              {t.text}
            </div>
          ))}
        </div>,
        document.body,
      )}
    </Ctx.Provider>
  );
}

export function useToast(): ToastFn {
  return useContext(Ctx);
}
