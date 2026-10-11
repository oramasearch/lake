/** The phone's full-screen sheet, ported from rev-os (lake#561): the page lock shared by Ask Orama
 * and every `Modal sheet`, where the sheet goes, and how far a field is scrolled to clear the
 * keyboard. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { createPageLock, revealBy, type PageEnv, type StyleLike } from "../src/lib/sheet.ts";
import { sheetFrame } from "../src/lib/phone.ts";

function style(init: Partial<StyleLike> = {}): StyleLike & { vars: Record<string, string> } {
  const vars: Record<string, string> = {};
  return {
    position: "",
    top: "",
    left: "",
    right: "",
    overflow: "",
    ...init,
    vars,
    setProperty: (k: string, v: string) => {
      vars[k] = v;
    },
    removeProperty: (k: string) => {
      delete vars[k];
    },
  };
}

function page(scrollY: number) {
  const root = { style: style({ overflow: "auto" }), dataset: {} as Record<string, string | undefined> };
  const body = { style: style({ position: "relative", top: "2px" }) };
  const scrolls: number[] = [];
  const env: PageEnv = { root, body, scrollY: () => scrollY, scrollTo: (_x, y) => scrolls.push(y) };
  return { root, body, scrolls, lock: createPageLock(() => env) };
}

test("the page behind is pinned at its scroll position and put back exactly", () => {
  const p = page(420);
  p.root.style.setProperty("--vv-height", "508px");
  const unlock = p.lock.lock();
  assert.equal(p.body.style.position, "fixed");
  assert.equal(p.body.style.top, "-420px");
  assert.equal(p.root.style.overflow, "hidden");
  assert.equal(p.root.dataset.sheet, "open");
  unlock();
  assert.equal(p.body.style.position, "relative");
  assert.equal(p.body.style.top, "2px");
  assert.equal(p.root.style.overflow, "auto");
  assert.equal(p.root.dataset.sheet, undefined);
  assert.equal(p.root.style.vars["--vv-height"], undefined, "the viewport variables go with the last sheet");
  assert.deepEqual(p.scrolls, [420], "back at the same place");
});

test("a sheet over another never unlocks the page under the first", () => {
  const p = page(300);
  const ask = p.lock.lock();
  const dialog = p.lock.lock();
  assert.equal(p.lock.depth(), 2);
  dialog();
  assert.equal(p.body.style.position, "fixed", "still locked for Ask Orama");
  assert.deepEqual(p.scrolls, []);
  dialog(); // a second call does nothing
  assert.equal(p.lock.depth(), 1);
  ask();
  assert.equal(p.body.style.position, "relative");
  assert.deepEqual(p.scrolls, [300]);
});

test("the sheet is the visual viewport, and the keyboard is what it lost", () => {
  // iPhone 390×844, keyboard closed, open, and Safari scrolling the visual viewport up.
  assert.deepEqual(sheetFrame({ height: 844, offsetTop: 0 }, 844), { top: 0, height: 844, keyboard: 0 });
  assert.deepEqual(sheetFrame({ height: 508, offsetTop: 0 }, 844), { top: 0, height: 508, keyboard: 336 });
  assert.deepEqual(sheetFrame({ height: 508.4, offsetTop: 120 }, 844), { top: 120, height: 508, keyboard: 216 });
  // No visualViewport: the layout viewport, no keyboard known (100dvh does the rest).
  assert.deepEqual(sheetFrame(null, 700), { top: 0, height: 700, keyboard: 0 });
});

test("a field is scrolled just clear of the keyboard and of what is pinned over it", () => {
  const view = { top: 60, bottom: 450 };
  assert.equal(revealBy({ top: 200, bottom: 260 }, view), 0, "already in view");
  assert.equal(revealBy({ top: 500, bottom: 560 }, view), 122, "below: up to the bottom edge less the margin");
  assert.equal(revealBy({ top: 20, bottom: 80 }, view), -52, "above (under a pinned header): down to the top");
  // A reply box taller than the room keeps its top in view rather than its bottom.
  assert.equal(revealBy({ top: 300, bottom: 900 }, view), 228);
});
