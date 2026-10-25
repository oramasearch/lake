/** Coupon codes (lake#1156): a `?code=` link's code is kept through sign-up (Clerk's round trip, a
 * `?next=` redirect that drops the query, an email link in another tab), taken out of the address,
 * redeemed once on the first signed-in page, and forgotten after a day or once Lake has answered;
 * and the operator's create form. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  COUPON_KEEP_MS,
  captureCouponCode,
  codeInQuery,
  couponInput,
  dollarsAsMicros,
  forgetCouponCode,
  normalizeCode,
  pendingCouponCode,
  personalAccount,
  queryWithoutCode,
  redemptionIsSettled,
  type CouponStorage,
} from "../src/lib/coupon.ts";

function memory(): CouponStorage & { map: Map<string, string> } {
  const map = new Map<string, string>();
  return {
    map,
    getItem: (key) => map.get(key) ?? null,
    setItem: (key, value) => void map.set(key, value),
    removeItem: (key) => void map.delete(key),
  };
}

test("codes are normalized as Lake stores them", () => {
  assert.equal(normalizeCode("lake50"), "LAKE50");
  assert.equal(normalizeCode(" Lake-50 "), "LAKE50");
  assert.equal(normalizeCode("ab"), null);
  assert.equal(normalizeCode("LAKE_50"), null);
  assert.equal(normalizeCode("<script>"), null);
  assert.equal(normalizeCode(null), null);
  assert.equal(codeInQuery("?utm_source=google&code=lake50"), "LAKE50");
  assert.equal(codeInQuery("?next=%2F"), null);
});

test("the code is kept through sign-up and leaves the address", () => {
  const storage = memory();
  const now = 1_800_000_000_000;
  // Opened from the ad (the apex picker carried the query to the region).
  assert.equal(captureCouponCode("?code=lake50&utm_source=google", storage, now), "LAKE50");
  assert.equal(queryWithoutCode("?code=lake50&utm_source=google"), "?utm_source=google");
  assert.equal(queryWithoutCode("?code=LAKE50"), "");
  // Sign-up: /sign-in?next=/ and Clerk's own pages carry no code; the stored one waits.
  assert.equal(captureCouponCode("?next=%2F", storage, now + 1000), null);
  assert.equal(pendingCouponCode(storage, now + 60_000), "LAKE50");
  // A later link replaces it.
  captureCouponCode("?code=SPRING25", storage, now + 120_000);
  assert.equal(pendingCouponCode(storage, now + 130_000), "SPRING25");
  // Redeemed (or refused by Lake): forgotten.
  forgetCouponCode(storage);
  assert.equal(pendingCouponCode(storage, now + 140_000), null);
});

test("a kept code is forgotten after a day, and nonsense is never kept", () => {
  const storage = memory();
  const now = 1_800_000_000_000;
  captureCouponCode("?code=LAKE50", storage, now);
  assert.equal(pendingCouponCode(storage, now + COUPON_KEEP_MS - 1), "LAKE50");
  assert.equal(pendingCouponCode(storage, now + COUPON_KEEP_MS), null);
  assert.equal(storage.map.size, 0, "the expired code is removed");
  assert.equal(captureCouponCode("?code=%3Cimg%3E", storage, now), null);
  assert.equal(storage.map.size, 0);
  storage.setItem("lake-coupon", "not json");
  assert.equal(pendingCouponCode(storage, now), null);
  storage.setItem("lake-coupon", JSON.stringify({ code: "LAKE50", at: now + 10_000 }));
  assert.equal(pendingCouponCode(storage, now), null, "a time in the future is not trusted");
});

test("Lake's answer settles the code; a network failure keeps it for the next page", () => {
  assert.equal(redemptionIsSettled({ status: 409, code: "conflict" }), true);
  assert.equal(redemptionIsSettled({ status: 404, code: "not_found" }), true);
  assert.equal(redemptionIsSettled({ status: 429, code: "rate_limited" }), true);
  assert.equal(redemptionIsSettled({ status: 0, code: "network" }), false);
  assert.equal(redemptionIsSettled({ status: 503, code: "store_unavailable" }), false);
  assert.equal(redemptionIsSettled(null), true);
});

test("the code goes to the person's own account", () => {
  assert.equal(personalAccount([{ id: "org", personal: false }, { id: "mine", personal: true }]), "mine");
  assert.equal(personalAccount([{ id: "org", personal: false }]), null);
  assert.equal(personalAccount(undefined), null);
});

test("the create form becomes create_coupon's input", () => {
  assert.equal(dollarsAsMicros("50"), 50_000_000);
  assert.equal(dollarsAsMicros("$12.34"), 12_340_000);
  assert.equal(dollarsAsMicros("0"), null);
  assert.equal(dollarsAsMicros("12.345"), null);
  assert.equal(dollarsAsMicros("10000.01"), null);
  const base = { code: "lake-50", dollars: "50", days: "90", expires: "2026-12-31", max: "500", newOnly: true, campaign: "google-2026-11" };
  assert.deepEqual(couponInput(base), {
    input: { new_accounts_only: true, code: "LAKE50", amount_micros: 50_000_000, credit_days: 90, expires_at: "2026-12-31T23:59:59Z", max_redemptions: 500, campaign: "google-2026-11" },
    problem: null,
  });
  // A generated code: no code sent.
  assert.equal("code" in (couponInput({ ...base, code: "" }).input ?? {}), false);
  assert.match(couponInput({ ...base, code: "x" }).problem ?? "", /4 to 32/);
  assert.match(couponInput({ ...base, dollars: "" }).problem ?? "", /amount/);
  assert.match(couponInput({ ...base, days: "0" }).problem ?? "", /days/);
  assert.match(couponInput({ ...base, max: "0" }).problem ?? "", /most redemptions/);
  assert.match(couponInput({ ...base, campaign: "Google" }).problem ?? "", /campaign/i);
  assert.equal(couponInput({ ...base, newOnly: false }).input?.new_accounts_only, false);
});
