/** One click opens (#230): a second press soon after is the same intent, never its opposite.
 * `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { backdropMayClose, isRepeat, SECOND_CLICK_MS } from "../src/lib/clicks.ts";

test("a press within the double-click interval, or the second of a double-click, repeats the first", () => {
  assert.equal(isRepeat(1000, 1000 + 150), true);
  assert.equal(isRepeat(1000, 1000 + SECOND_CLICK_MS - 1), true);
  assert.equal(isRepeat(1000, 1000 + SECOND_CLICK_MS), false);
  assert.equal(isRepeat(1000, 1000 + 2000, 2), true);
  assert.equal(isRepeat(-Infinity, 5), false);
});

test("a backdrop closes only on a deliberate press after the dialog has appeared", () => {
  const press = { startedOnBackdrop: true, endedOnBackdrop: true, detail: 1 };
  assert.equal(backdropMayClose(press, 1000, 1000 + 200), false, "the opener's second click");
  assert.equal(backdropMayClose({ ...press, detail: 2 }, 1000, 1000 + 900), false, "a double-click");
  assert.equal(backdropMayClose({ ...press, startedOnBackdrop: false }, 1000, 3000), false, "a drag out of the dialog");
  assert.equal(backdropMayClose({ ...press, endedOnBackdrop: false }, 1000, 3000), false, "a press that ends inside");
  assert.equal(backdropMayClose(press, 1000, 3000), true);
});
