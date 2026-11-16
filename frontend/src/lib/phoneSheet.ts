// SPDX-License-Identifier: MIT

/** The phone's full-screen sheet, ported from rev-os (lake#561): Ask Orama and every `Modal sheet`
 * use the same mechanics. docs/ui/mobile-web.md explains each rule.
 *
 * iOS Safari does not resize the page when the keyboard opens: it shrinks and scrolls the *visual*
 * viewport. So while a sheet is open on a phone its box is sized to the visual viewport
 * (`--vv-top`, `--vv-height` on <html>, updated on every visualViewport resize and scroll): what is
 * pinned to its bottom (a composer, a Save button) then sits just above the keyboard, and `100dvh`
 * is the fallback where there is no visualViewport. The page behind is locked (body fixed at its
 * scroll position) and restored exactly on close (`lib/sheet.ts`, counted). */
import { type RefObject, useEffect, useState } from "react";
import { sheetFrame } from "./phone";
import { createPageLock, revealBy } from "./sheet";

/** Below the `md` breakpoint: a phone, held either way. */
export const PHONE_QUERY = "(max-width: 767px)";

function matches(query: string): boolean {
  try {
    return typeof window !== "undefined" && window.matchMedia(query).matches;
  } catch {
    return false;
  }
}

export function useMedia(query: string): boolean {
  const [on, setOn] = useState(() => matches(query));
  useEffect(() => {
    const mq = window.matchMedia(query);
    const change = () => setOn(mq.matches);
    change();
    mq.addEventListener("change", change);
    return () => mq.removeEventListener("change", change);
  }, [query]);
  return on;
}

export const useIsPhone = () => useMedia(PHONE_QUERY);
/** A touch screen: Return makes a new line and the send button sends, as in messaging apps. */
export const useCoarsePointer = () => useMedia("(pointer: coarse)");

const pageLock = createPageLock(() => ({
  root: document.documentElement,
  body: document.body,
  scrollY: () => window.scrollY,
  scrollTo: (x, y) => window.scrollTo(x, y),
}));

/** Marks what should come into view with a focused field inside it (a box with its button), so the
 * button is above the keyboard too. */
export const KEEP_VISIBLE = "data-keep-visible";
/** Marks what is pinned over a sheet's scroller (a sticky header), so revealing keeps clear of it. */
export const PINNED_TOP = "data-pinned-top";

/** Scroll `scroller` so the focused field inside it (or its `data-keep-visible` group) is in view. */
export function revealFocused(scroller: HTMLElement | null) {
  if (!scroller) return;
  const active = document.activeElement as HTMLElement | null;
  if (!active || !scroller.contains(active) || active === scroller) return;
  const target = (active.closest(`[${KEEP_VISIBLE}]`) as HTMLElement | null) ?? active;
  if (!scroller.contains(target)) return;
  const box = scroller.getBoundingClientRect();
  let top = box.top;
  scroller.querySelectorAll<HTMLElement>(`[${PINNED_TOP}]`).forEach((el) => {
    top = Math.max(top, el.getBoundingClientRect().bottom);
  });
  const t = target.getBoundingClientRect();
  const by = revealBy({ top: t.top, bottom: t.bottom }, { top, bottom: box.bottom });
  if (by) scroller.scrollTop += by;
}

/** While `active`: the visual viewport as CSS variables, and the page behind locked. Returns the
 * keyboard's height (0 when closed). With `reveal`, the focused field inside that scroller is
 * brought above the keyboard whenever the keyboard opens or moves, and when a field is focused. */
export function usePhoneSheet(active: boolean, reveal?: RefObject<HTMLElement | null>): number {
  const [keyboard, setKeyboard] = useState(0);
  useEffect(() => {
    if (!active) return;
    const root = document.documentElement;
    const vv = window.visualViewport;
    let frame = 0;
    const later = () => {
      if (!reveal) return;
      cancelAnimationFrame(frame);
      // After the browser has laid the sheet out at its new size.
      frame = requestAnimationFrame(() => revealFocused(reveal.current));
    };
    const update = () => {
      const f = sheetFrame(vv, window.innerHeight);
      root.style.setProperty("--vv-height", `${f.height}px`);
      root.style.setProperty("--vv-top", `${f.top}px`);
      setKeyboard(f.keyboard);
      later();
    };
    update();
    vv?.addEventListener("resize", update);
    vv?.addEventListener("scroll", update);
    window.addEventListener("resize", update);
    const scroller = reveal?.current ?? null;
    scroller?.addEventListener("focusin", later);
    const unlock = pageLock.lock();
    return () => {
      cancelAnimationFrame(frame);
      vv?.removeEventListener("resize", update);
      vv?.removeEventListener("scroll", update);
      window.removeEventListener("resize", update);
      scroller?.removeEventListener("focusin", later);
      unlock();
      setKeyboard(0);
    };
  }, [active, reveal]);
  return keyboard;
}
