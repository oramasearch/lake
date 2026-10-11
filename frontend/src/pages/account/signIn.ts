/** Where a sign-in page goes once lake's `lake_session` cookie is set, and the identity calls'
 * shared options. Sign-in pages navigate with a full `location.assign`, so the whole app (and the
 * `viewer` read) starts again as the new session. */
import { ApiError } from "@/lib/api";

export { landingAfterSignIn } from "@/pages/ask/home";

/** web_ui_invitation_note: Clerk's sign-up is restricted while lake is in development. */
export const INVITATION_NOTE = "Accounts are added by invitation while lake is in development.";

/** Identity capabilities belong to no workspace: call them unprefixed. */
export const NO_WORKSPACE = { workspace: null } as const;

/** A same-origin path: one leading slash, never `//host` or `/\host`. */
export function sameOriginPath(value: string | null | undefined): value is string {
  return !!value && value.startsWith("/") && !value.startsWith("//") && !value.startsWith("/\\");
}

/** The page to open after signing in: `?return_to` when it is a Ship app's `/apps/authorize`
 * (set by that page), else `?next` when it is a same-origin path, else the page's own default. */
export function returnTarget(search: string, fallback: string): string {
  const params = new URLSearchParams(search);
  const returnTo = params.get("return_to");
  if (sameOriginPath(returnTo) && (returnTo === "/apps/authorize" || returnTo.startsWith("/apps/authorize?"))) return returnTo;
  const next = params.get("next");
  if (sameOriginPath(next)) return next;
  return fallback;
}

/** Only the return parameters of a query (`next`, `return_to`), for links between sign-in pages. */
export function returnQuery(search: string): string {
  const params = new URLSearchParams(search);
  const kept = new URLSearchParams();
  for (const key of ["next", "return_to"]) {
    const value = params.get(key);
    if (value) kept.set(key, value);
  }
  const text = kept.toString();
  return text ? `?${text}` : "";
}

let leaving = false;

/** Whether this page is already leaving after a sign-in (a later redirect must not race it). */
export function isLeaving(): boolean {
  return leaving;
}

/** Leave for `target` with a full page load, so the new session is read from the start. */
export function leaveFor(target: string, replace = false) {
  leaving = true;
  if (replace) window.location.replace(target);
  else window.location.assign(target);
}

/** The message of a failed call. */
export function messageOf(error: unknown): string {
  if (error instanceof ApiError || error instanceof Error) return error.message;
  return "The request failed.";
}
