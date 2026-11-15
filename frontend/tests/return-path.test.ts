// SPDX-License-Identifier: MIT

/** A return path after sign-in stays on this origin (LSR-184, lake#990). `npm test`. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { sameOriginPath } from "../src/lib/returnPath.ts";

test("same-origin paths are accepted", () => {
  for (const path of ["/", "/design/pipeline?step=a", "/apps/authorize?x=1#y"]) {
    assert.equal(sameOriginPath(path), true, path);
  }
});

test("anything a browser could read as another host is refused", () => {
  for (const path of [
    "",
    "https://example.test/",
    "//example.test/",
    "/\\example.test/",
    "/\t/example.test/",
    "/\n/example.test/",
    "/\r/example.test",
    "/\u0000/example.test",
    "/a\\b",
    "/\u007f/x",
  ]) {
    assert.equal(sameOriginPath(path), false, JSON.stringify(path));
  }
});
