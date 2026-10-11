/** Billing (lake#564, lake#635): amounts from millionths, rates in a person's units, a line as usage ×
 * rate = cost, the months a bill covers, invoice states, what is owed, the price list editor's rows,
 * and the rail that shows Billing only where the lake bills. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { DIMENSIONS, LIST_RATES, billedNote, invoiceTone, lineSentence, monthLabel, months, money, owed, paymentSummary, payOnline, priceLabel, providerStatus, quantityLabel, rateRows, ratesInput, recordedByHand } from "../src/pages/billing/billing.ts";
import { billingVisible } from "../src/routes/nav.ts";

test("an amount is in its currency, to the cent unless a price has fractions of one", () => {
  assert.equal(money(20_000_000, "USD"), "$20.00");
  assert.equal(money(2_500_000, "USD"), "$2.50");
  assert.equal(money(0, "USD"), "$0.00");
  assert.equal(money(400, "USD"), "$0.0004");
  assert.equal(money(1_234_567_890, "EUR"), "€1,234.57");
});

test("a rate reads in the dimension's unit, and a line is usage × rate = cost", () => {
  assert.equal(priceLabel({ unit_micros: 1_000_000, per: 1000, unit: "calls" }, "USD"), "$1.00 per 1,000 calls");
  assert.equal(priceLabel({ unit_micros: 100_000, per: 1024 ** 3, unit: "bytes" }, "USD"), "$0.10 per 1 GiB");
  assert.equal(priceLabel({ unit_micros: 2_000_000, per: 1_000_000, unit: "tokens" }, "USD"), "$2.00 per 1M tokens");
  assert.equal(quantityLabel({ quantity: 3 * 1024 ** 3, unit: "bytes" }), "3 GiB");
  assert.equal(lineSentence({ quantity: 3500, billable: 3500, unit: "calls", unit_micros: 1_000_000, per: 1000, amount_micros: 3_500_000 }, "USD"), "3,500 calls × $1.00 per 1,000 calls = $3.50");
  assert.equal(lineSentence({ quantity: 10, billable: 10, unit: "calls", unit_micros: 0, per: 1, amount_micros: 0 }, "USD"), "10 calls, not charged");
});

test("time is priced and billed by the minute, on the month's total (lake#637)", () => {
  assert.equal(priceLabel({ unit_micros: 5_000, per: 60, unit: "seconds" }, "USD"), "$0.005 per minute");
  assert.equal(priceLabel({ unit_micros: 1_350, per: 60, unit: "instance_seconds" }, "USD"), "$0.00135 per instance-minute");
  assert.equal(priceLabel({ unit_micros: 7_200, per: 60, unit: "seconds" }, "USD"), "$0.0072 per minute");
  assert.equal(priceLabel({ unit_micros: 300_000, per: 3600, unit: "seconds" }, "USD"), "$0.30 per hour");
  assert.equal(priceLabel({ unit_micros: 200_000, per: 2_628_000, unit: "gib_seconds" }, "USD"), "$0.20 per GiB-month");
  // 100 one-second builds: 100 seconds used, billed as 2 minutes.
  const builds = { quantity: 100, billable: 120, unit: "seconds", unit_micros: 5_000, per: 60, amount_micros: 10_000 };
  assert.equal(billedNote(builds), "billed as 2 min");
  assert.equal(lineSentence(builds, "USD"), "2 min × $0.005 per minute = $0.01");
  assert.equal(billedNote({ quantity: 120, billable: 120, unit: "seconds" }), "");
  for (const rate of LIST_RATES.filter((r) => ["build_compute", "run_compute", "speech", "instances"].includes(r.dimension))) {
    assert.equal(rate.per, 60, `${rate.dimension} is priced per minute`);
  }
});

test("a bill covers a UTC month, and January's month before is December", () => {
  assert.deepEqual(months(new Date("2026-10-10T15:00:00Z")), { current: "2026-10", previous: "2026-09" });
  assert.deepEqual(months(new Date("2027-01-01T00:30:00Z")), { current: "2027-01", previous: "2026-12" });
  assert.equal(monthLabel("2026-10"), "October 2026");
  assert.equal(monthLabel("soon"), "soon");
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

test("the price list editor starts from the published list, or the list rates, one row a dimension", () => {
  assert.equal(DIMENSIONS.length, 10);
  assert.equal(LIST_RATES.length, 10, "the list rates price every dimension");
  const fresh = rateRows(null);
  assert.equal(fresh.length, 10);
  assert.deepEqual(fresh[0], { dimension: "storage", rate: "0.04", per: String(1024 ** 3) });
  assert.deepEqual(fresh.find((r) => r.dimension === "model_tokens"), { dimension: "model_tokens", rate: "9", per: "1000000" });
  const published = rateRows([{ dimension: "calls", unit_micros: 1_500_000, per: 1_000_000 }]);
  assert.deepEqual(published.find((r) => r.dimension === "calls"), { dimension: "calls", rate: "1.5", per: "1000000" });
  assert.deepEqual(published.find((r) => r.dimension === "storage"), { dimension: "storage", rate: "", per: "1" }, "a dimension the list does not price starts empty");
});

test("the editor's rows become set_price_list's prices: millionths, whole units, empty rates left out", () => {
  assert.deepEqual(ratesInput(rateRows(null)), { prices: LIST_RATES });
  assert.deepEqual(
    ratesInput([
      { dimension: "calls", rate: "0.0004", per: "" },
      { dimension: "storage", rate: " ", per: "1" },
    ]),
    { prices: [{ dimension: "calls", unit_micros: 400, per: 1 }] },
  );
  assert.deepEqual(ratesInput([{ dimension: "calls", rate: "1", per: "1" }, { dimension: "speech", rate: "-1", per: "60" }]), { bad: 1 });
  assert.deepEqual(ratesInput([{ dimension: "calls", rate: "1", per: "0" }]), { bad: 0 });
  assert.deepEqual(ratesInput([{ dimension: "calls", rate: "1", per: "1.5" }]), { bad: 0 });
  assert.deepEqual(ratesInput([{ dimension: "calls", rate: "free", per: "1" }]), { bad: 0 });
});

test("Billing is in the rail only where the lake bills", () => {
  assert.equal(billingVisible({ billing: true }, { billing: true }), true);
  assert.equal(billingVisible({ billing: true }, { billing: false }), false);
  assert.equal(billingVisible({ billing: true }, {}), false);
  assert.equal(billingVisible({}, { billing: false }), true);
});

test("a due Stripe invoice is paid on Stripe's page; others are not", () => {
  const url = "https://invoice.stripe.com/i/acct_1/test_abc";
  assert.equal(payOnline({ state: "due", payment_url: url }), url);
  assert.equal(payOnline({ state: "paid", payment_url: url }), null, "a paid invoice has nothing to pay");
  assert.equal(payOnline({ state: "void", payment_url: url }), null);
  assert.equal(payOnline({ state: "due", payment_url: "" }), null, "an invoice paid outside lake");
  assert.equal(payOnline({ state: "due" }), null);
  assert.equal(payOnline({ state: "due", payment_url: "javascript:alert(1)" }), null, "only an https page");
  assert.equal(payOnline({ state: "due", payment_url: "http://invoice.stripe.com/i/x" }), null);
  assert.equal(recordedByHand({ provider: "manual" }), true);
  assert.equal(recordedByHand({ provider: "stripe" }), false);
});

test("Stripe's state of an invoice reads in words", () => {
  assert.equal(providerStatus(""), null);
  assert.equal(providerStatus(undefined), null);
  assert.deepEqual(providerStatus("open"), { label: "Awaiting payment", tone: "warning" });
  assert.deepEqual(providerStatus("payment_failed"), { label: "Payment failed", tone: "danger" });
  assert.deepEqual(providerStatus("paid"), { label: "Paid", tone: "success" });
  assert.deepEqual(providerStatus("uncollectible"), { label: "uncollectible", tone: "neutral" });
});

test("the Payment figure names the provider and says when card payment is not set up", () => {
  assert.deepEqual(paymentSummary({ name: "stripe", title: "Stripe", records_payments_by_hand: false, online_payment: true, reason: "" }), { value: "Stripe", sub: "pay each invoice on Stripe's page" });
  assert.deepEqual(paymentSummary({ name: "manual", title: "Invoice only", records_payments_by_hand: true, reason: "Card payment is not set up: no mount." }), { value: "Invoice only", sub: "card payment is not set up" });
  assert.deepEqual(paymentSummary({ name: "manual", title: "Invoice only", records_payments_by_hand: true }), { value: "Invoice only", sub: "Orama records each payment" });
});
