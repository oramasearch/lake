// SPDX-License-Identifier: MIT

/** A run report's at-risk evidence in words, never raw JSON (lake#581). `npm test`; the build runs
 * it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { effectLabel, evidenceOf, figure } from "../src/pages/train/evidence.ts";

test("evidence values become feature = value, and reasons carry their effect", () => {
  const evidence = evidenceOf({
    monthly_spend: 129,
    tenure_months: 3.25,
    plan: "basic",
    reason_codes: [
      { reason: "tenure_months", effect: 0.4213 },
      { reason: "support_tickets", effect: -0.05 },
    ],
  });
  assert.deepEqual(evidence.values, [
    { name: "monthly_spend", value: "129" },
    { name: "tenure_months", value: "3.25" },
    { name: "plan", value: "basic" },
  ]);
  assert.deepEqual(evidence.reasons, [
    { reason: "tenure_months", effect: 0.4213, label: "raises risk by 0.4213" },
    { reason: "support_tickets", effect: -0.05, label: "lowers risk by 0.05" },
  ]);
});

test("reason codes written as a JSON string or as names still read", () => {
  assert.deepEqual(evidenceOf({ reason_codes: '[{"reason":"spend","effect":0.1}]' }).reasons, [{ reason: "spend", effect: 0.1, label: "raises risk by 0.1" }]);
  assert.deepEqual(evidenceOf({ reason_codes: ["spend"] }).reasons, [{ reason: "spend", effect: null, label: "" }]);
  assert.deepEqual(evidenceOf('{"spend":1}').values, [{ name: "spend", value: "1" }]);
});

test("no evidence is no chips", () => {
  assert.deepEqual(evidenceOf(undefined), { values: [], reasons: [] });
  assert.deepEqual(evidenceOf([1, 2]), { values: [], reasons: [] });
  assert.deepEqual(evidenceOf("not json"), { values: [], reasons: [] });
});

test("figures read as numbers", () => {
  assert.equal(figure(1234567), "1,234,567");
  assert.equal(figure(0.123456), "0.1235");
  assert.equal(effectLabel(0), "");
  assert.equal(effectLabel(null), "");
});
