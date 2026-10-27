/** The app page's Change App split screen (lake#1241): it opens from the page's top and closes back
 * to it, the divider resizes within bounds, and the App's frame reloads once a confirmed change is
 * deployed and Ready. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { clampSplit, deployedAndReady, deployFailed, frameUrl, SPLIT_DEFAULT, SPLIT_MAX, SPLIT_MIN, splitAt, splitByKey, storedSplit } from "../src/pages/apps/changeSplit.ts";

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
  assert.equal(frameUrl("https://reports.us.lake.live"), "https://reports.us.lake.live");
  assert.equal(frameUrl("javascript:alert(1)"), "");
  assert.equal(frameUrl(undefined), "");
});
