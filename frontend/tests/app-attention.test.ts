// SPDX-License-Identifier: MIT

/** The Apps page counts a Degraded (crash-looping) app as needing attention (lake#1208). `npm test`;
 * the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { needsAttention } from "../src/pages/apps/attention.ts";

test("Failed and Degraded apps need attention; others do not", () => {
  assert.equal(needsAttention("Failed"), true);
  assert.equal(needsAttention("Degraded"), true);
  for (const phase of ["Running", "Pending", "Deploying", "Stopped", "Deleted", "", null, undefined]) {
    assert.equal(needsAttention(phase), false, String(phase));
  }
});

test("the Apps page's figure counts and links the apps needing attention", () => {
  const page = readFileSync(new URL("../src/pages/apps/AppsPage.tsx", import.meta.url), "utf8");
  assert.match(page, /<Stat label="Needs attention"/);
  assert.doesNotMatch(page, /<Stat label="Failed"/);
  assert.match(page, /to={appsHref\(ATTENTION_FILTER\)}/);
});
