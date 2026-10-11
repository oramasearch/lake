/** Which failed reads are asked again, and which stop polling (lake#575). `npm test`; the build
 * runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { isNotSetUp, pollUnlessFailed, shouldRetry, stopsPolling } from "../src/lib/errors.ts";

const failure = (status: number, code: string) => ({ status, code });

test("a 4xx is the answer and is not asked again", () => {
  assert.equal(shouldRetry(0, failure(400, "invalid_input")), false);
  assert.equal(shouldRetry(0, failure(404, "not_found")), false);
});

test("not_supported and store_unavailable are shown at once, never retried", () => {
  assert.equal(shouldRetry(0, failure(501, "not_supported")), false);
  assert.equal(shouldRetry(0, failure(503, "store_unavailable")), false);
});

test("another server or network failure is retried twice", () => {
  assert.equal(shouldRetry(0, failure(500, "store_error")), true);
  assert.equal(shouldRetry(1, failure(503, "workspace_not_ready")), true);
  assert.equal(shouldRetry(2, failure(0, "network")), false);
  assert.equal(shouldRetry(0, new Error("boom")), true);
});

test("not_supported says the feature is not set up", () => {
  assert.equal(isNotSetUp(failure(501, "not_supported")), true);
  assert.equal(isNotSetUp(failure(503, "store_unavailable")), false);
  assert.equal(isNotSetUp(null), false);
});

test("polling stops on an answer it cannot change and goes on otherwise", () => {
  const at = (error: unknown) => ({ state: { error } });
  const every = pollUnlessFailed<{ state: { error: unknown } }>(3000) as (q: { state: { error: unknown } }) => number | false;
  assert.equal(every(at(null)), 3000);
  assert.equal(every(at(failure(503, "store_unavailable"))), false);
  assert.equal(every(at(failure(501, "not_supported"))), false);
  assert.equal(every(at(failure(403, "unauthorized"))), false);
  assert.equal(every(at(failure(500, "store_error"))), 3000);
  assert.equal(stopsPolling(failure(500, "store_error")), false);
  const computed = pollUnlessFailed<{ state: { error: unknown } }>(() => 2000) as (q: { state: { error: unknown } }) => number | false;
  assert.equal(computed(at(null)), 2000);
  assert.equal(computed(at(failure(501, "not_supported"))), false);
  assert.equal(pollUnlessFailed(false), false);
  assert.equal(pollUnlessFailed(undefined), undefined);
});
