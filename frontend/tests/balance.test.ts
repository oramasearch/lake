/** The prepaid balance (lake#1114): the banner every page shows while the balance is low, negative or
 * blocked, the Add funds link, the top-up amounts, the history of credits and charges, and what the
 * page says after Stripe. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { addFundsPath, balanceTone, bannerFor, dollarsToMicros, grantInput, historyRows, microsToDollars, returnNotice, topUpRule, type Balance } from "../src/pages/billing/balance.ts";

const base: Balance = {
  customer_id: "acct",
  currency: "USD",
  balance_micros: 15_000_000,
  overdraft_micros: 50_000_000,
  limit_micros: -50_000_000,
  state: "ok",
  add_micros: 20_000_000,
  add_funds_url: "https://lake.live/billing/add-funds?customer_id=acct",
  first_top_up_micros: 20_000_000,
  minimum_top_up_micros: 20_000_000,
  first_top_up_at: "2026-10-24T00:00:00Z",
};

test("no banner while the balance is fine, or a paid order covers the account", () => {
  assert.equal(bannerFor(base), null);
  assert.equal(bannerFor(null), null);
  assert.equal(bannerFor({ ...base, state: "blocked", reason: "overdraft", exempt: "paid_order" }), null);
});

test("the banner says the balance, how much to add and what happens at the limit", () => {
  const low = bannerFor({ ...base, state: "low", balance_micros: 4_000_000 });
  assert.equal(low?.title, "Your Balance Is Low");
  assert.match(low!.text, /\$4\.00/);
  assert.match(low!.text, /Add \$20\.00/);
  const negative = bannerFor({ ...base, state: "negative", balance_micros: -5_000_000 });
  assert.equal(negative?.tone, "warn");
  assert.match(negative!.text, /-\$5\.00/);
  assert.match(negative!.text, /-\$50\.00/);
  const blocked = bannerFor({ ...base, state: "blocked", reason: "overdraft", balance_micros: -52_000_000, add_micros: 62_000_000 });
  assert.equal(blocked?.title, "Paid Work Is Stopped");
  assert.match(blocked!.text, /keeps running/);
  assert.match(blocked!.text, /\$62\.00/);
  const first = bannerFor({ ...base, state: "blocked", reason: "first_top_up", balance_micros: 0, first_top_up_at: "" });
  assert.equal(first?.title, "Add Funds to Start");
  assert.equal(first?.tone, "info");
});

test("every Add funds link is the one console page for the account", () => {
  assert.equal(addFundsPath("a b"), "/billing/add-funds?customer_id=a%20b");
  assert.equal(balanceTone("ok"), "success");
  assert.equal(balanceTone("low"), "warning");
  assert.equal(balanceTone("blocked"), "danger");
});

test("amounts are whole cents in dollars", () => {
  assert.equal(dollarsToMicros("25"), 25_000_000);
  assert.equal(dollarsToMicros("$1,000.5"), 1_000_500_000);
  assert.equal(dollarsToMicros("25.505"), null);
  assert.equal(dollarsToMicros("abc"), null);
  assert.equal(microsToDollars(20_000_000), "20");
  assert.equal(microsToDollars(25_500_000), "25.50");
});

test("the first top-up is the first amount; later ones at least the minimum", () => {
  const first = topUpRule({ ...base, first_top_up_at: "" });
  assert.equal(first.first, true);
  assert.equal(first.amount, 20_000_000);
  assert.match(first.problem(50_000_000) ?? "", /first top-up is \$20\.00/);
  assert.equal(first.problem(20_000_000), null);
  const later = topUpRule({ ...base, add_micros: 62_000_000 });
  assert.equal(later.amount, 62_000_000, "what brings the balance back");
  assert.match(later.problem(19_000_000) ?? "", /at least \$20\.00/);
  assert.equal(later.problem(135_000_000), null);
  assert.match(later.problem(null) ?? "", /dollars and cents/);
});

test("the history lists credits and monthly charges, newest first", () => {
  const rows = historyRows(
    [
      { id: "1", kind: "top_up", amount_micros: 20_000_000, provider: "stripe", reference: "cs_1", created_at: "2026-09-03T10:00:00Z" },
      { id: "2", kind: "auto_top_up", amount_micros: 20_000_000, provider: "stripe", reference: "pi_1", created_at: "2026-10-12T10:00:00Z" },
    ],
    [
      { period: "2026-10", amount_micros: 7_000_000, price_list_id: "p", invoice_id: "", refreshed_at: "" },
      { period: "2026-09", amount_micros: 11_000_000, price_list_id: "p", invoice_id: "inv", refreshed_at: "" },
    ],
  );
  assert.deepEqual(rows.map((r) => r.label), ["Auto top-up", "Usage, October 2026", "Top-up", "Usage, September 2026"]);
  assert.equal(rows[1].amount_micros, -7_000_000);
  assert.equal(rows[3].detail, "Invoiced");
});

test("the page says what happened after Stripe sends the person back", () => {
  assert.equal(returnNotice(new URLSearchParams("top_up=done"))?.tone, "ok");
  assert.match(returnNotice(new URLSearchParams("top_up=cancelled"))!.text, /Nothing was charged/);
  assert.match(returnNotice(new URLSearchParams("auto_top_up=saved"))!.text, /Card saved/);
  assert.equal(returnNotice(new URLSearchParams("customer_id=x")), null);
});

// Credits from Orama (lake#1134).
test("a credit from Orama is listed with its reason, expiry and what is left, and its unused part stops on expiry", () => {
  const rows = historyRows(
    [
      { id: "g1", kind: "grant", label: "Credit from Orama", amount_micros: 25_000_000, provider: "orama", reference: "grant-a", created_at: "2026-10-01T10:00:00Z", reason: "Design partner", expires_at: "2026-12-31T23:59:59Z", ended_at: "", end_reason: "", forfeited_micros: 0, unused_micros: 15_000_000 },
      { id: "g2", kind: "grant", amount_micros: 10_000_000, provider: "orama", reference: "grant-b", created_at: "2026-09-01T10:00:00Z", reason: "Prize", expires_at: "2026-09-30T23:59:59Z", ended_at: "2026-09-30T23:59:59Z", end_reason: "expired", forfeited_micros: 4_000_000 },
    ],
    [],
  );
  assert.deepEqual(rows.map((r) => r.label), ["Credit from Orama", "Credit from Orama expired", "Credit from Orama"]);
  assert.equal(rows[0].detail, "Design partner · expires 2026-12-31 · $15.00 left");
  assert.equal(rows[0].grantId, "g1");
  assert.equal(rows[1].amount_micros, -4_000_000);
  assert.equal(rows[2].detail, "Prize · expired 2026-09-30");
  assert.equal(rows[2].grantId, undefined, "an ended credit is not revoked again");
});

test("the banner says when a credit from Orama is used up", () => {
  const used = bannerFor({ ...base, state: "blocked", reason: "first_top_up", first_top_up_at: "", first_grant_at: "2026-10-01T00:00:00Z", balance_micros: 0 });
  assert.equal(used?.title, "Your Credit Is Used Up");
  assert.match(used!.text, /Add \$20\.00/);
});

test("the Give credit form names the account by ID or email and checks the amount, reason and last day", () => {
  const now = new Date("2026-10-24T12:00:00Z");
  const byId = grantInput({ account: " acct ", dollars: "25", reason: " Design partner ", expires: "2026-12-31" }, now);
  assert.equal(byId.problem, null);
  assert.deepEqual(byId.input, { amount_micros: 25_000_000, reason: "Design partner", customer_id: "acct", expires_at: "2026-12-31T23:59:59Z" });
  const byEmail = grantInput({ account: "Pat@Example.com", dollars: "5.50", reason: "Prize", expires: "" }, now);
  assert.deepEqual(byEmail.input, { amount_micros: 5_500_000, reason: "Prize", email: "Pat@Example.com" });
  assert.match(grantInput({ account: "", dollars: "25", reason: "x", expires: "" }, now).problem!, /account/);
  assert.match(grantInput({ account: "a", dollars: "0", reason: "x", expires: "" }, now).problem!, /amount/);
  assert.match(grantInput({ account: "a", dollars: "10000.01", reason: "x", expires: "" }, now).problem!, /\$10,000/);
  assert.match(grantInput({ account: "a", dollars: "25", reason: "  ", expires: "" }, now).problem!, /why/);
  assert.match(grantInput({ account: "a", dollars: "25", reason: "x", expires: "2026-10-23" }, now).problem!, /future/);
});
