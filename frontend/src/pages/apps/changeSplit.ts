/** The app page's Change App split screen (lake#1241, docs/ui/apps.md#change-app-split-screen): the
 * conversation's share of the width, remembered in this browser, and when the running App's frame
 * reloads after a confirmed change. Pure, so `npm test` covers it (tests/change-app-split.test.ts). */

export const SPLIT_KEY = "lake-change-app-split";
export const SPLIT_DEFAULT = 0.4;
export const SPLIT_MIN = 0.25;
export const SPLIT_MAX = 0.75;

/** The conversation's share of the width, kept between a quarter and three quarters. */
export function clampSplit(fraction: number): number {
  if (!Number.isFinite(fraction)) return SPLIT_DEFAULT;
  return Math.min(SPLIT_MAX, Math.max(SPLIT_MIN, fraction));
}

/** The share a pointer at `clientX` gives, within a container starting at `left` and `width` wide. */
export function splitAt(clientX: number, left: number, width: number): number {
  return width > 0 ? clampSplit((clientX - left) / width) : SPLIT_DEFAULT;
}

/** The share after an arrow key on the divider: 2% a press, 10% with Shift. */
export function splitByKey(fraction: number, key: string, shift: boolean): number | null {
  const step = shift ? 0.1 : 0.02;
  if (key === "ArrowLeft") return clampSplit(fraction - step);
  if (key === "ArrowRight") return clampSplit(fraction + step);
  if (key === "Home") return SPLIT_MIN;
  if (key === "End") return SPLIT_MAX;
  return null;
}

/** A stored share, or the default for nothing stored or anything unreadable. */
export function storedSplit(value: string | null): number {
  const parsed = Number(value);
  return value !== null && value !== "" && Number.isFinite(parsed) ? clampSplit(parsed) : SPLIT_DEFAULT;
}

/** The App's address the frame may show: an http(s) URL, else nothing (no frame, and Change App
 * says why). */
export function frameUrl(url: unknown): string {
  return typeof url === "string" && /^https?:\/\//.test(url) ? url : "";
}

type AppView = { phase?: unknown; ready?: unknown; generation?: unknown; observed_generation?: unknown };

/** Whether the App, read after a confirmed change was applied, runs that change: the operator has
 * observed its newest generation, and it is Ready. Until then the frame keeps the old App. */
export function deployedAndReady(app: AppView | undefined): boolean {
  if (!app) return false;
  const generation = Number(app.generation ?? 0);
  const observed = Number(app.observed_generation ?? 0);
  if (observed < generation) return false;
  const phase = String(app.phase ?? "");
  return app.ready === true || phase === "Running" || phase === "Ready";
}

/** Whether the App failed to come up after the change (the frame is not reloaded onto it; the page
 * says so and the person can still reload by hand). */
export function deployFailed(app: AppView | undefined): boolean {
  if (!app) return false;
  const phase = String(app.phase ?? "");
  return Number(app.observed_generation ?? 0) >= Number(app.generation ?? 0) && (phase === "Failed" || phase === "Degraded");
}
