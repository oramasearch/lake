/** Billing (lake#564): amounts from millionths, prices in a person's units, the months a bill
 * covers, what choosing a plan does, invoice states, what is owed, the plan form's numbers, and the
 * rail that shows Billing only where the lake bills. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { choiceOf, choiceSentence, includedLabel, invoiceTone, microsOf, monthLabel, months, money, owed, priceInput, priceLabel, quantityLabel } from "../src/pages/billing/billing.ts";
import { billingVisible } from "../src/routes/nav.ts";

test("an amount is in its currency, to the cent unless a price has fractions of one", () => {
  assert.equal(money(20_000_000, "USD"), "$20.00");
  assert.equal(money(2_500_000, "USD"), "$2.50");
  assert.equal(money(0, "USD"), "$0.00");
  assert.equal(money(400, "USD"), "$0.0004");
  assert.equal(money(1_234_567_890, "EUR"), "€1,234.57");
});

test("a price reads in the dimension's unit, with what the plan includes", () => {
  assert.equal(priceLabel({ unit_micros: 1_000_000, per: 1000, unit: "calls" }, "USD"), "$1.00 per 1,000 calls");
  assert.equal(priceLabel({ unit_micros: 100_000, per: 1024 ** 3, unit: "bytes" }, "USD"), "$0.10 per 1 GiB");
  assert.equal(priceLabel({ unit_micros: 2_000_000, per: 1_000_000, unit: "tokens" }, "USD"), "$2.00 per 1M tokens");
  assert.equal(includedLabel({ included: 1000, unit: "calls" }), "1,000 calls included");
  assert.equal(includedLabel({ included: 0, unit: "calls" }), "");
  assert.equal(quantityLabel({ kind: "plan", quantity: 1, unit: "month" }), "1 month");
  assert.equal(quantityLabel({ kind: "usage", quantity: 3 * 1024 ** 3, unit: "bytes" }), "3 GiB");
});

test("a bill covers a UTC month, and January's month before is December", () => {
  assert.deepEqual(months(new Date("2026-10-10T15:00:00Z")), { current: "2026-10", previous: "2026-09" });
  assert.deepEqual(months(new Date("2027-01-01T00:30:00Z")), { current: "2027-01", previous: "2026-12" });
  assert.equal(monthLabel("2026-10"), "October 2026");
  assert.equal(monthLabel("soon"), "soon");
});

test("choosing a plan starts it now, changes it next month, or cancels a pending change", () => {
  const on = (plan: string, next?: string) => ({
    current: { effective_from: "2026-10-01", plan_id: plan, plan: null },
    next: next ? { effective_from: "2026-11-01", plan_id: next, plan: null } : null,
  });
  assert.equal(choiceOf(null, "team"), "start");
  assert.equal(choiceOf({ current: null, next: null }, "team"), "start");
  assert.equal(choiceOf(on("free"), "team"), "next");
  assert.equal(choiceOf(on("free"), "free"), "current");
  assert.equal(choiceOf(on("free", "team"), "free"), "cancel");
  assert.match(choiceSentence("next", "Team", "2026-11"), /from November 2026/);
  assert.match(choiceSentence("start", "Team", "2026-11"), /start of this month/);
  assert.match(choiceSentence("cancel", "Free", "2026-11"), /cancelled/);
});

test("an invoice's state is a chip, and what is owed adds up the due ones by currency", () => {
  assert.equal(invoiceTone("due"), "warning");
  assert.equal(invoiceTone("paid"), "success");
  assert.equal(invoiceTone("void"), "neutral");
  assert.deepEqual(
    owed([
      { state: "due", total_micros: 1_000_000, currency: "USD" },
      { state: "paid", total_micros: 5_000_000, currency: "USD" },
      { state: "due", total_micros: 2_000_000, currency: "USD" },
      { state: "void", total_micros: 9_000_000, currency: "USD" },
    ]),
    [{ currency: "USD", micros: 3_000_000 }],
  );
  assert.deepEqual(owed([]), []);
});

test("the plan form turns a person's numbers into millionths and whole units", () => {
  assert.deepEqual(priceInput({ dimension: "calls", price: "1.00", per: "1000", included: "1000" }), { dimension: "calls", unit_micros: 1_000_000, per: 1000, included: 1000 });
  assert.deepEqual(priceInput({ dimension: "calls", price: "0.0004", per: "", included: "" }), { dimension: "calls", unit_micros: 400, per: 1, included: 0 });
  assert.equal(priceInput({ dimension: "calls", price: "-1", per: "1", included: "0" }), null);
  assert.equal(priceInput({ dimension: "calls", price: "1", per: "0", included: "0" }), null);
  assert.equal(priceInput({ dimension: "calls", price: "1", per: "1.5", included: "0" }), null);
  assert.equal(priceInput({ dimension: "", price: "1", per: "1", included: "0" }), null);
  assert.equal(microsOf("19.99"), 19_990_000);
  assert.equal(microsOf(""), null);
  assert.equal(microsOf("free"), null);
});

test("Billing is in the rail only where the lake bills", () => {
  assert.equal(billingVisible({ billing: true }, { billing: true }), true);
  assert.equal(billingVisible({ billing: true }, { billing: false }), false);
  assert.equal(billingVisible({ billing: true }, {}), false);
  assert.equal(billingVisible({}, { billing: false }), true);
});
