// SPDX-License-Identifier: MIT

/**
 * Ship → Search Index (zetta#1243, lake#596): a hit shows the matched row's fields, in the index's
 * field order, instead of the workspace's internal row id.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { fieldText, hitFields } from "../src/pages/ship/search.ts";

test("a hit's fields come in the index's order, then any others", () => {
  const hit = { id: "1fb0a49b", score: 0.5, fields: { plan: "enterprise", account_id: "a-7", note: null, seats: 40 } };
  assert.deepEqual(hitFields(hit, ["account_id", "plan"]), [
    ["account_id", "a-7"],
    ["plan", "enterprise"],
    ["note", "—"],
    ["seats", "40"],
  ]);
  assert.deepEqual(hitFields(hit), [
    ["plan", "enterprise"],
    ["account_id", "a-7"],
    ["note", "—"],
    ["seats", "40"],
  ]);
});

test("a hit without fields has none to show, so the page falls back to its id", () => {
  assert.deepEqual(hitFields({ id: "doc-7", score: 2.5 }), [], "an older engine");
  assert.deepEqual(hitFields({ id: "doc-7", score: 2.5, fields: {} }), [], "fields: []");
  assert.deepEqual(hitFields({ id: "doc-7", fields: ["a"] }), []);
  assert.deepEqual(hitFields(null), []);
  assert.equal(fieldText({ a: 1 }), '{"a":1}');
  assert.equal(fieldText(undefined), "—");
});
