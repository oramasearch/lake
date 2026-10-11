/** Which failed reads are worth asking again, and which say a feature is not set up (lake#575).
 * Pure, so `npm test` covers it (tests/errors.test.ts); `api.ts` hands `shouldRetry` to every query.
 *
 * - A 4xx is the answer (not found, not allowed, a bad id): show it at once.
 * - `not_supported` (501) means this lake cannot do it: asking again changes nothing until an
 *   operator configures it, so the page says the feature is not set up, once.
 * - `store_unavailable` (503) means a store or dependency is not reachable now. Lake answers it at
 *   once and asking again in the same second rarely helps, so the page shows it with Retry and
 *   does not ask again on its own.
 * - Anything else that failed on the server or the network is retried twice. That includes a 503
 *   lake did not write (the edge or the connection answered; `api.ts` gives it the code
 *   `unavailable`), which is what a reader saw after creating a drive (lake#573). */

/** What a failed read carries: the HTTP status (0 for the network) and the error code. */
export type Failure = { status?: number; code?: string };

/** Codes that a retry cannot fix. */
export const NOT_RETRIED = new Set(["not_supported", "store_unavailable"]);

function failureOf(error: unknown): Failure | null {
  return error && typeof error === "object" ? (error as Failure) : null;
}

/** TanStack Query's `retry`: whether to ask again after `count` failures. */
export function shouldRetry(count: number, error: unknown): boolean {
  const failure = failureOf(error);
  if (failure) {
    const status = failure.status ?? 0;
    if (status >= 400 && status < 500) return false;
    if (failure.code && NOT_RETRIED.has(failure.code)) return false;
  }
  return count < 2;
}

/** Whether a failed read says this lake does not have the feature (`not_supported`). */
export function isNotSetUp(error: unknown): boolean {
  return failureOf(error)?.code === "not_supported";
}

/** Whether a failed read is an answer polling cannot change: a 4xx, `not_supported` or
 * `store_unavailable`. A polled read stops on it, and the page shows it with Retry. */
export function stopsPolling(error: unknown): boolean {
  const failure = failureOf(error);
  if (!failure) return false;
  const status = failure.status ?? 0;
  return (status >= 400 && status < 500) || (!!failure.code && NOT_RETRIED.has(failure.code));
}

type Interval<Q> = number | false | undefined | ((query: Q) => number | false | undefined);

/** A query's `refetchInterval` that stops while its last read failed with an answer polling cannot
 * change (`stopsPolling`); a Retry that succeeds starts it again. Every read hook wraps its
 * interval with this, so no page polls a feature that is not set up (lake#575). */
export function pollUnlessFailed(interval: number | false): false | ((query: { state: { error: unknown } }) => number | false | undefined);
export function pollUnlessFailed<Q extends { state: { error: unknown } }>(interval: Interval<Q>): Interval<Q>;
export function pollUnlessFailed<Q extends { state: { error: unknown } }>(interval: Interval<Q>): Interval<Q> {
  if (interval === undefined || interval === false) return interval;
  return (query: Q) => {
    if (stopsPolling(query.state.error)) return false;
    return typeof interval === "function" ? interval(query) : interval;
  };
}

/** Where an answer that is not lake's came from, for the error a person sees and reports: the
 * `server` header and Cloudflare's `cf-ray` when there is one (lake#573). Empty when neither. */
export function edgeSource(server: string | null, ray: string | null): string {
  const parts = [server?.trim(), ray?.trim() ? `ray ${ray.trim()}` : ""].filter(Boolean);
  return parts.join(", ");
}
