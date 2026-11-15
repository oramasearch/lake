// SPDX-License-Identifier: MIT

/** One click opens.
 *
 * Right after a page loads the browser is busy, so a dialog or a panel can take a moment to
 * appear, and people click again — or double-click out of habit. That second press used to land
 * on what the first had just opened: on a dialog's backdrop, which closed it, or on a toggle
 * (the Assistant), which turned it back off. So a press that follows the one that opened
 * something within the double-click interval is the same intent, never its opposite. */

/** The double-click interval, as most systems set it. */
export const SECOND_CLICK_MS = 500;

/** Whether a press at `now` repeats one at `last` (ms on the same clock): the second press of a
 * double-click (`detail` > 1), or any press within the interval. */
export function isRepeat(last: number, now: number, detail = 1): boolean {
  return detail > 1 || now - last < SECOND_CLICK_MS;
}

/** Whether a press on a backdrop may close what it covers: it began and ended on the backdrop
 * (not a drag out of the dialog), is no part of a double-click, and comes after the interval
 * that follows the dialog's appearing. */
export function backdropMayClose(press: { startedOnBackdrop: boolean; endedOnBackdrop: boolean; detail: number }, openedAt: number, now: number): boolean {
  return press.startedOnBackdrop && press.endedOnBackdrop && !isRepeat(openedAt, now, press.detail);
}
