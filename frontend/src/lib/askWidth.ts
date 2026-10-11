/** The Ask Orama panel's width: resizable like rev-os's assistant pane. Default 560px, between
 * 320px and 70% of the window, remembered in this browser. The shell reads it to push the page
 * aside while the page keeps room to read. */
import { useCallback, useEffect, useState } from "react";

export const ASK_DEFAULT_WIDTH = 560;
const KEY = "lake-ask-width";
const MIN = 320;
let dragging = false;

/** Whether the panel is being dragged (the shell turns its margin transition off meanwhile). */
export function resizingAsk(): boolean {
  return dragging;
}
export function setResizingAsk(value: boolean) {
  dragging = value;
  // The panel animates expand and collapse, never a drag (app.css).
  if (typeof document !== "undefined") document.documentElement.toggleAttribute("data-resizing-ask", value);
}

export function clampAskWidth(width: number): number {
  const max = Math.max(MIN, Math.floor(window.innerWidth * 0.7));
  return Math.min(max, Math.max(MIN, Math.round(width)));
}

export function useAskWidth(): [number, (width: number) => void] {
  const [width, setState] = useState<number>(() => {
    try {
      const stored = Number(localStorage.getItem(KEY));
      return clampAskWidth(stored > 0 ? stored : ASK_DEFAULT_WIDTH);
    } catch {
      return clampAskWidth(ASK_DEFAULT_WIDTH);
    }
  });
  const setWidth = useCallback((next: number) => {
    const clamped = clampAskWidth(next);
    setState(clamped);
    try {
      localStorage.setItem(KEY, String(clamped));
    } catch {
      /* private mode: the width lasts for the tab */
    }
  }, []);
  useEffect(() => {
    const onResize = () => setState((w) => clampAskWidth(w));
    window.addEventListener("resize", onResize);
    return () => window.removeEventListener("resize", onResize);
  }, []);
  return [width, setWidth];
}
