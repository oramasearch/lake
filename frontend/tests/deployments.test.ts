// SPDX-License-Identifier: MIT

/** Ship → Deployments asks once on a lake without the platform mirror (lake#605). `npm test`; the
 * build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { asksRepository, deploymentsSetUp } from "../src/pages/apps/deployments.ts";

const notSupported = { isSuccess: false, isError: true, error: { status: 501, code: "not_supported" } };
const answered = { isSuccess: true, isError: false, error: null };
const pending = { isSuccess: false, isError: false, error: null };
const failed = { isSuccess: false, isError: true, error: { status: 500, code: "store_error" } };

test("server_info's deployments flag is read as said, and nothing when it is missing", () => {
  assert.equal(deploymentsSetUp({ deployments: false }), false);
  assert.equal(deploymentsSetUp({ deployments: true }), true);
  assert.equal(deploymentsSetUp({}), undefined);
  assert.equal(deploymentsSetUp(undefined), undefined);
});

test("a lake that says Deployments are not set up is asked about no repository", () => {
  for (const index of [0, 1, 15]) assert.equal(asksRepository(index, false, answered), false);
});

test("a lake that says they are set up is asked about every repository at once", () => {
  for (const index of [0, 1, 15]) assert.equal(asksRepository(index, true, undefined), true);
});

test("a lake that does not say is asked about the first repository alone until it answers", () => {
  assert.equal(asksRepository(0, undefined, undefined), true);
  assert.equal(asksRepository(1, undefined, undefined), false);
  assert.equal(asksRepository(1, undefined, pending), false);
  assert.equal(asksRepository(1, undefined, notSupported), false, "not set up: nothing more is asked");
  assert.equal(asksRepository(1, undefined, answered), true);
  assert.equal(asksRepository(1, undefined, failed), true, "another failure is that repository's own");
});
