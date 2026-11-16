// SPDX-License-Identifier: MIT

/** A full-screen sheet on a phone, as plain logic, ported from rev-os (lake#561). No React and no
 * real DOM, so `npm test` covers it. The hooks in `lib/phoneSheet.ts` apply it to Ask Orama and to
 * every `Modal sheet`; docs/ui/mobile-web.md explains why.
 *
 * - The page lock: while any sheet is open the page behind is pinned (body fixed at its scroll
 *   position, the root's overflow hidden) and put back exactly when the last one closes. It is
 *   counted, so a sheet opened over another never restores the page under the first.
 * - Revealing: how far a sheet's scroller moves so the field being typed in (and its button)
 *   sit above the keyboard and below anything pinned over the scroller. */

/** The few style properties the lock touches, as on a CSSStyleDeclaration. */
export type StyleLike = {
  position: string;
  top: string;
  left: string;
  right: string;
  overflow: string;
  setProperty(name: string, value: string): void;
  removeProperty(name: string): string | void;
};

export type PageEnv = {
  root: { style: StyleLike; dataset: Record<string, string | undefined> };
  body: { style: StyleLike };
  scrollY: () => number;
  scrollTo: (x: number, y: number) => void;
};

export type PageLock = {
  /** Lock the page (the first lock pins it); returns the unlock, safe to call twice. */
  lock(): () => void;
  depth(): number;
};

export function createPageLock(env: () => PageEnv): PageLock {
  let depth = 0;
  let saved: { scrollY: number; position: string; top: string; left: string; right: string; overflow: string } | null = null;

  const pin = () => {
    const { root, body, scrollY } = env();
    const y = scrollY();
    saved = { scrollY: y, position: body.style.position, top: body.style.top, left: body.style.left, right: body.style.right, overflow: root.style.overflow };
    body.style.position = "fixed";
    body.style.top = `-${y}px`;
    body.style.left = "0";
    body.style.right = "0";
    root.style.overflow = "hidden";
    root.dataset.sheet = "open";
  };

  const release = () => {
    const { root, body, scrollTo } = env();
    if (!saved) return;
    body.style.position = saved.position;
    body.style.top = saved.top;
    body.style.left = saved.left;
    body.style.right = saved.right;
    root.style.overflow = saved.overflow;
    delete root.dataset.sheet;
    root.style.removeProperty("--vv-height");
    root.style.removeProperty("--vv-top");
    scrollTo(0, saved.scrollY);
    saved = null;
  };

  return {
    lock() {
      if (depth === 0) pin();
      depth += 1;
      let done = false;
      return () => {
        if (done) return;
        done = true;
        depth -= 1;
        if (depth === 0) release();
      };
    },
    depth: () => depth,
  };
}

/** How far to scroll a sheet's scroller (positive: down) so a target sits inside its visible
 * part — the scroller's box less what is pinned over its top and bottom — with a little room.
 * 0 when it is already in view. A target taller than the room keeps its top in view. */
export function revealBy(target: { top: number; bottom: number }, view: { top: number; bottom: number }, margin = 12): number {
  const top = view.top + margin;
  const bottom = view.bottom - margin;
  if (target.top < top) return Math.round(target.top - top);
  if (target.bottom > bottom) return Math.round(Math.min(target.bottom - bottom, target.top - top));
  return 0;
}
