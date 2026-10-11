/** lake#573: a read answered 503 by the edge is retried once, after a short pause. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { edgeSource, readRetryDelay } from "../src/lib/retry.ts";

test("a 503 the edge answered is retried once after a short pause", () => {
  assert.equal(readRetryDelay(503, false, null, 0), 400);
  assert.equal(readRetryDelay(503, false, null, 1), null, "only once");
  assert.equal(readRetryDelay(502, false, null, 0), null, "a 502 is not a 503");
  assert.equal(readRetryDelay(200, true, null, 0), null);
});

test("lake's own 503 is retried only when it says when to come back", () => {
  assert.equal(readRetryDelay(503, true, null, 0), null, "not set up: a retry changes nothing");
  assert.equal(readRetryDelay(503, true, "1", 0), 1000);
  assert.equal(readRetryDelay(503, true, "0", 0), 250, "never sooner than 250 ms");
  assert.equal(readRetryDelay(503, true, "30", 0), 2000, "never later than 2 s");
  assert.equal(readRetryDelay(503, true, "soon", 0), 400);
});

test("an edge answer names its server and ray", () => {
  assert.equal(edgeSource("cloudflare", "a489c8b9682d804a-YVR"), "cloudflare, ray a489c8b9682d804a-YVR");
  assert.equal(edgeSource("nginx", null), "nginx");
  assert.equal(edgeSource(null, null), "");
});
