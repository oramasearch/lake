/** When a read is worth sending once more (lake#573, docs/ui/web-app.md#a-read-answered-503). No
 * React and no `@/` imports, so `tests/retry.test.ts` runs it under Node directly.
 *
 * A 503 that is not lake's own answer (no `{ok: false, error}` body: the edge or the connection in
 * between answered) is retried once after a short pause, and so is lake's own 503 when it carries
 * `Retry-After` (a workspace store moving or handed off, a full server). Lake's 503 without it
 * means something is not set up, which a retry does not change. Actions are never retried here: a
 * write may have happened. */
export function readRetryDelay(status: number, lakeAnswered: boolean, retryAfter: string | null, attempt: number): number | null {
  if (status !== 503 || attempt > 0) return null;
  if (lakeAnswered && retryAfter === null) return null;
  const seconds = retryAfter === null ? NaN : Number(retryAfter);
  if (Number.isFinite(seconds) && seconds >= 0) return Math.min(2000, Math.max(250, seconds * 1000));
  return 400;
}

/** Where an answer that is not lake's came from, for the error a person sees and reports: the
 * `server` header and Cloudflare's `cf-ray` when there is one. Empty when lake answered. */
export function edgeSource(server: string | null, ray: string | null): string {
  const parts = [server?.trim(), ray?.trim() ? `ray ${ray.trim()}` : ""].filter(Boolean);
  return parts.join(", ");
}
