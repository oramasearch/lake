// SPDX-License-Identifier: MIT

/** The app page's Change App split screen (lake#1241): it opens from the page's top and closes back
 * to it, the divider resizes within bounds, and the App's frame reloads once a confirmed change is
 * deployed and Ready. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { clampSplit, deployedAndReady, deployFailed, frameUrl, SETTLE_RELOADS_MS, SPLIT_DEFAULT, SPLIT_MAX, SPLIT_MIN, splitAt, splitByKey, storedSplit } from "../src/pages/apps/changeSplit.ts";

const page = readFileSync(new URL("../src/pages/apps/AppPage.tsx", import.meta.url), "utf8");
const split = readFileSync(new URL("../src/pages/apps/ChangeAppSplit.tsx", import.meta.url), "utf8");

test("Change App at the top of the app page opens the split, and closing returns to the page", () => {
  assert.match(page, /onClick=\{\(\) => setChanging\(true\)\}/);
  assert.match(page, /<Bot size=\{14\} aria-hidden \/> Change App/);
  assert.match(page, /changing = params\.get\("change"\) === "1"/);
  assert.match(page, /\{changing && <ChangeAppSplit [^>]*onClose=\{\(\) => setChanging\(false\)\}/);
  assert.match(page, /next\.delete\("change"\)/);
  // Escape closes too.
  assert.match(split, /event\.key === "Escape"[^\n]*onClose\(\)/);
});

test("the conversation is on the left and the running App in a frame on the right", () => {
  const left = split.indexOf("<AppConversation ");
  const frame = split.indexOf("<iframe ");
  assert.ok(left > 0 && frame > left);
  assert.match(split, /<iframe key=\{frameKey\} src=\{url\}/);
  assert.match(split, /onApplied=\{onApplied\}/);
  assert.match(split, /target="_blank" rel="noopener noreferrer"/);
  assert.match(split, /Open in a new tab/);
  assert.match(split, /onClick=\{reload\}/);
});

test("the frame reloads after a confirmed change once the App's newest generation is Ready", () => {
  // The reload is a new key on the frame, made only when the App runs the change.
  assert.match(split, /if \(deployedAndReady\(next\)\) \{\s*setWaiting\(false\);\s*reload\(\);/);
  assert.equal(deployedAndReady(undefined), false);
  assert.equal(deployedAndReady({ generation: 4, observed_generation: 3, phase: "Running", ready: true }), false, "the old generation is still what runs");
  assert.equal(deployedAndReady({ generation: 4, observed_generation: 4, phase: "Deploying", ready: false }), false);
  assert.equal(deployedAndReady({ generation: 4, observed_generation: 4, phase: "Running", ready: true }), true);
  assert.equal(deployedAndReady({ generation: 4, observed_generation: 4, phase: "Ready" }), true);
  assert.equal(deployFailed({ generation: 4, observed_generation: 4, phase: "Degraded" }), true);
  assert.equal(deployFailed({ generation: 4, observed_generation: 3, phase: "Failed" }), false, "an older failure is not this change's");
});

test("after Ready the frame reloads again over the next minute, since Ready can come before the new version answers (lake#1259)", () => {
  // On lake-dev the operator read Ready 2 s after a deploy; the old version answered until 7 s, a
  // 502 at 7 s, the new one from 8 s (orama-operator#1049). One reload at Ready showed the old App.
  assert.ok(SETTLE_RELOADS_MS.length >= 3);
  assert.ok(SETTLE_RELOADS_MS[0] <= 5000, "the first reload again comes within seconds");
  assert.ok(SETTLE_RELOADS_MS.at(-1)! >= 60000, "and the last a minute after Ready");
  assert.deepEqual([...SETTLE_RELOADS_MS].sort((a, b) => a - b), [...SETTLE_RELOADS_MS], "in order");
  // Ready starts the settling reloads; each step reloads, the last ends it, and the page says so.
  assert.match(split, /if \(deployedAndReady\(next\)\) \{\s*setWaiting\(false\);\s*reload\(\);\s*setSettling\(true\);/);
  assert.match(split, /SETTLE_RELOADS_MS\.map\(\(delay, index\) =>\s*window\.setTimeout\(\(\) => \{\s*reload\(\);\s*if \(index === SETTLE_RELOADS_MS\.length - 1\) setSettling\(false\);/);
  assert.match(split, /timers\.forEach\(\(timer\) => window\.clearTimeout\(timer\)\)/);
  assert.match(split, /the App reloads a few times while the new version takes over\. Reload if it still shows the old one\./);
});

test("the divider resizes between a quarter and three quarters, and is remembered", () => {
  assert.equal(clampSplit(0.1), SPLIT_MIN);
  assert.equal(clampSplit(0.9), SPLIT_MAX);
  assert.equal(clampSplit(Number.NaN), SPLIT_DEFAULT);
  assert.equal(splitAt(600, 100, 1000), 0.5);
  assert.equal(splitAt(0, 100, 1000), SPLIT_MIN);
  assert.equal(splitAt(5, 0, 0), SPLIT_DEFAULT);
  assert.equal(splitByKey(0.5, "ArrowLeft", false), 0.48);
  assert.equal(splitByKey(0.5, "ArrowRight", true), 0.6);
  assert.equal(splitByKey(0.5, "Home", false), SPLIT_MIN);
  assert.equal(splitByKey(0.5, "End", false), SPLIT_MAX);
  assert.equal(splitByKey(0.5, "a", false), null);
  assert.equal(storedSplit(null), SPLIT_DEFAULT);
  assert.equal(storedSplit("junk"), SPLIT_DEFAULT);
  assert.equal(storedSplit("0.6"), 0.6);
  assert.match(split, /role="separator"/);
});

test("only an http(s) App address is framed", () => {
  assert.equal(frameUrl("https://reports.us.lake.example.com"), "https://reports.us.lake.example.com");
  assert.equal(frameUrl("javascript:alert(1)"), "");
  assert.equal(frameUrl(undefined), "");
});
