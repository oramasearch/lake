/** Billing (lake#564, lake#635): amounts from millionths, rates in a person's units, a line as usage ×
 * rate = cost, the months a bill covers, invoice states, what is owed, the price list editor's rows,
 * and the rail that shows Billing only where the lake bills. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { DIMENSIONS, GPU_CLASSES, LIST_RATES, billedNote, invoiceTone, lineSentence, monthLabel, months, money, owed, paymentSummary, payOnline, priceLabel, providerStatus, quantityLabel, rateRows, ratesInput, recordedByHand, rowTitle } from "../src/pages/billing/billing.ts";
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
  for (const rate of LIST_RATES.filter((r) => ["speech", "gpu"].includes(r.dimension))) {
    assert.equal(rate.per, 60, `${rate.dimension} is priced per minute`);
  }
});

test("compute is priced per vCPU-minute, GiB-minute and GPU-minute, a rate per GPU class (lake#643)", () => {
  assert.equal(priceLabel({ unit_micros: 1_500, per: 60_000, unit: "mcpu_seconds" }, "USD"), "$0.0015 per vCPU-minute");
  assert.equal(priceLabel({ unit_micros: 170, per: 61_440, unit: "mib_seconds" }, "USD"), "$0.00017 per GiB-minute");
  assert.equal(priceLabel({ unit_micros: 10_800, per: 60, unit: "gpu_seconds" }, "USD"), "$0.0108 per GPU-minute");
  assert.equal(priceLabel({ unit_micros: 90_000, per: 3_600_000, unit: "mcpu_seconds" }, "USD"), "$0.09 per vCPU-hour");
  // Half a vCPU for 61 seconds: 30,500 mCPU-seconds, billed as one vCPU-minute.
  const cpu = { quantity: 30_500, billable: 60_000, unit: "mcpu_seconds", unit_micros: 1_500, per: 60_000, amount_micros: 1_500 };
  assert.equal(billedNote(cpu), "billed as 1 vCPU-min");
  for (const rate of LIST_RATES.filter((r) => r.dimension === "cpu")) assert.equal(rate.per, 60_000, "per vCPU-minute");
  for (const rate of LIST_RATES.filter((r) => r.dimension === "ram" || r.dimension === "vram")) assert.equal(rate.per, 61_440, "per GiB-minute");
  // The retired time dimensions are not on the list any more.
  for (const retired of ["instances", "build_compute", "run_compute"]) {
    assert.equal(DIMENSIONS.some((d) => d.name === retired), false, retired);
    assert.equal(LIST_RATES.some((r) => r.dimension === retired), false, retired);
  }
  // Every GPU class the editor offers has a list rate, and so does a GPU of no known class.
  for (const name of GPU_CLASSES) assert.ok(LIST_RATES.some((r) => r.dimension === "gpu" && r.class === name), name);
  assert.ok(LIST_RATES.some((r) => r.dimension === "gpu" && !r.class));
  assert.equal(rowTitle({ dimension: "gpu", class: "h100" }), "GPU (H100)");
  assert.equal(rowTitle({ dimension: "gpu", class: "" }), "GPU, other classes");
  assert.equal(rowTitle({ dimension: "cpu", class: "" }), "CPU");
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

test("the price list editor starts from the published list, or the list rates, one row a dimension and GPU class", () => {
  assert.equal(DIMENSIONS.length, 12);
  const fresh = rateRows(null);
  // Twelve dimensions, plus four GPU classes each for GPU and GPU memory.
  assert.equal(fresh.length, 12 + 2 * GPU_CLASSES.length);
  assert.deepEqual(fresh[0], { dimension: "cpu", class: "", rate: "0.0015", per: "60000" });
  assert.deepEqual(fresh.find((r) => r.dimension === "gpu" && r.class === "h100"), { dimension: "gpu", class: "h100", rate: "0.07875", per: "60" });
  assert.deepEqual(fresh.find((r) => r.dimension === "vram" && r.class === "l4"), { dimension: "vram", class: "l4", rate: "", per: "1" }, "GPU memory has one rate for every class");
  assert.deepEqual(fresh.find((r) => r.dimension === "model_tokens"), { dimension: "model_tokens", class: "", rate: "9", per: "1000000" });
  const published = rateRows([
    { dimension: "calls", unit_micros: 1_500_000, per: 1_000_000 },
    { dimension: "gpu", class: "b200", unit_micros: 120_000, per: 60 },
    { dimension: "instances", unit_micros: 1_350, per: 60 },
  ]);
  assert.deepEqual(published.find((r) => r.dimension === "calls"), { dimension: "calls", class: "", rate: "1.5", per: "1000000" });
  assert.deepEqual(published.find((r) => r.dimension === "storage"), { dimension: "storage", class: "", rate: "", per: "1" }, "a dimension the list does not price starts empty");
  assert.deepEqual(published.find((r) => r.class === "b200"), { dimension: "gpu", class: "b200", rate: "0.12", per: "60" }, "a class the list names gets its row");
  assert.equal(published.some((r) => r.dimension === "instances"), false, "a retired dimension is not carried over");
});

test("the editor's rows become set_price_list's prices: millionths, whole units, classes, empty rates left out", () => {
  assert.deepEqual(ratesInput(rateRows(null)), { prices: LIST_RATES.slice().sort((a, b) => order(a) - order(b)) });
  assert.deepEqual(
    ratesInput([
      { dimension: "calls", class: "", rate: "0.0004", per: "" },
      { dimension: "storage", class: "", rate: " ", per: "1" },
      { dimension: "gpu", class: "l4", rate: "0.0108", per: "60" },
    ]),
    { prices: [{ dimension: "calls", unit_micros: 400, per: 1 }, { dimension: "gpu", class: "l4", unit_micros: 10_800, per: 60 }] },
  );
  assert.deepEqual(ratesInput([{ dimension: "calls", class: "", rate: "1", per: "1" }, { dimension: "speech", class: "", rate: "-1", per: "60" }]), { bad: 1 });
  assert.deepEqual(ratesInput([{ dimension: "calls", class: "", rate: "1", per: "0" }]), { bad: 0 });
  assert.deepEqual(ratesInput([{ dimension: "calls", class: "", rate: "1", per: "1.5" }]), { bad: 0 });
  assert.deepEqual(ratesInput([{ dimension: "calls", class: "", rate: "free", per: "1" }]), { bad: 0 });
});

// The editor's row order: dimensions as DIMENSIONS lists them, each followed by its GPU classes.
function order(rate: { dimension: string; class?: string }): number {
  const rows = rateRows(null);
  return rows.findIndex((r) => r.dimension === rate.dimension && r.class === (rate.class ?? ""));
}

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
