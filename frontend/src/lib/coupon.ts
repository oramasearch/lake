// SPDX-License-Identifier: MIT

/**
 * A coupon code carried through sign-up (lake#1156, docs/ui/balance.md#coupon-codes). An ad links to
 * `lake.live/?code=LAKE50`; the apex picker carries the query to the chosen region. The web app
 * keeps the code in this browser's localStorage for a day (it survives Clerk's sign-up round trip,
 * an email link opened in another tab, and the `?next=` redirects that do not keep the query), takes
 * it out of the address, and redeems it on the first signed-in page (`CouponWelcome`). Kept pure so
 * `npm test` can pin it; nothing here calls Lake.
 */

/** The localStorage key and how long a carried code is kept. */
export const COUPON_KEY = "lake-coupon";
export const COUPON_KEEP_MS = 24 * 60 * 60 * 1000;

/** The part of `Storage` this uses, so tests can pass a map. */
export type CouponStorage = Pick<Storage, "getItem" | "setItem" | "removeItem">;

/** A code as Lake stores it: letters upper-cased, blanks and hyphens dropped; null when what is left
 * is not 4 to 32 letters and digits. The same rule as the server's `coupon_code_normalized`. */
export function normalizeCode(typed: string | null | undefined): string | null {
  if (!typed) return null;
  const code = typed.replace(/[\s-]/g, "").toUpperCase();
  return /^[A-Z0-9]{4,32}$/.test(code) ? code : null;
}

/** The code in a query string (`?code=`), normalized, or null. */
export function codeInQuery(search: string): string | null {
  return normalizeCode(new URLSearchParams(search).get("code"));
}

/** The query without `code`: what the address keeps once the code is stored ("" when nothing is
 * left, else with its leading `?`). */
export function queryWithoutCode(search: string): string {
  const params = new URLSearchParams(search);
  params.delete("code");
  const text = params.toString();
  return text ? `?${text}` : "";
}

/** Keep the code a page was opened with (`?code=`), replacing an older one. Answers the code kept,
 * or null when the query had none (or one that is not a code). */
export function captureCouponCode(search: string, storage: CouponStorage, now = Date.now()): string | null {
  const code = codeInQuery(search);
  if (!code) return null;
  try {
    storage.setItem(COUPON_KEY, JSON.stringify({ code, at: now }));
  } catch {
    /* storage refused (private mode): the code is simply not carried */
  }
  return code;
}

/** The code waiting to be redeemed, or null when none is kept or it is older than a day (an
 * expired one is forgotten). */
export function pendingCouponCode(storage: CouponStorage, now = Date.now()): string | null {
  let kept: { code?: unknown; at?: unknown } | null = null;
  try {
    kept = JSON.parse(storage.getItem(COUPON_KEY) ?? "null");
  } catch {
    kept = null;
  }
  const code = typeof kept?.code === "string" ? normalizeCode(kept.code) : null;
  const at = typeof kept?.at === "number" ? kept.at : NaN;
  if (!code || !(now - at >= 0 && now - at < COUPON_KEEP_MS)) {
    forgetCouponCode(storage);
    return null;
  }
  return code;
}

export function forgetCouponCode(storage: CouponStorage) {
  try {
    storage.removeItem(COUPON_KEY);
  } catch {
    /* ignore */
  }
}

/** Whether a failed redemption is Lake's answer (refused, limited, not this account's), so the code
 * is forgotten; a network failure keeps it for the next page, within the day. */
export function redemptionIsSettled(error: { status?: number; code?: string } | null | undefined): boolean {
  if (!error) return true;
  return !(error.status === 0 || error.code === "network" || (error.status ?? 0) >= 500);
}

/** The account a carried code is redeemed for: the person's own (personal) account. */
export function personalAccount(spaces: { id: string; personal: boolean }[] | undefined): string | null {
  return spaces?.find((space) => space.personal)?.id ?? null;
}

/** Dollars typed in the create form as whole-cent micros, or null. */
export function dollarsAsMicros(text: string): number | null {
  const trimmed = text.trim().replace(/^\$/, "");
  if (!/^\d+(\.\d{1,2})?$/.test(trimmed)) return null;
  const micros = Math.round(Number(trimmed) * 100) * 10_000;
  return micros >= 10_000 && micros <= 10_000_000_000 ? micros : null;
}

/** The create form's fields as `create_coupon`'s input, or the first problem. */
export function couponInput(form: {
  code: string;
  dollars: string;
  days: string;
  expires: string;
  max: string;
  newOnly: boolean;
  campaign: string;
}): { input: Record<string, unknown> | null; problem: string | null } {
  const input: Record<string, unknown> = { new_accounts_only: form.newOnly };
  if (form.code.trim()) {
    const code = normalizeCode(form.code);
    if (!code) return { input: null, problem: "A code is 4 to 32 letters and digits." };
    input.code = code;
  }
  const micros = dollarsAsMicros(form.dollars);
  if (micros === null) return { input: null, problem: "Enter an amount from $0.01 to $10,000." };
  input.amount_micros = micros;
  const days = Number(form.days || "90");
  if (!Number.isInteger(days) || days < 1 || days > 3650) return { input: null, problem: "The credit lasts 1 to 3,650 days." };
  input.credit_days = days;
  if (form.expires) {
    // The last day the code works, to its end in UTC.
    input.expires_at = `${form.expires}T23:59:59Z`;
  }
  if (form.max.trim()) {
    const max = Number(form.max);
    if (!Number.isInteger(max) || max < 1) return { input: null, problem: "The most redemptions is a whole number, 1 or more." };
    input.max_redemptions = max;
  }
  const campaign = form.campaign.trim();
  if (campaign) {
    if (!/^[a-z0-9][a-z0-9._-]{0,63}$/.test(campaign)) return { input: null, problem: "A campaign is lower-case letters, digits, '.', '_' or '-'." };
    input.campaign = campaign;
  }
  return { input, problem: null };
}
