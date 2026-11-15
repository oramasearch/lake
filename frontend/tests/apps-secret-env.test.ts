// SPDX-License-Identifier: MIT

/** An app's secret environment names a lake secret (lake#625): the stored reference, what the
 * Environment table shows, and the form's set_app_env input. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { envInput, parseLakeSecretRef, secretEnvLabel } from "../src/pages/apps/secretEnv.ts";

test("a reference names a mount, a secret (which may hold / and #) and a field", () => {
  assert.deepEqual(parseLakeSecretRef("lake-secret://m1/payments/db#prod#DATABASE_URL"), {
    mount_id: "m1",
    name: "payments/db#prod",
    field: "DATABASE_URL",
  });
  for (const bad of ["db-url", "db/password", "lake-secret://m1/name", "lake-secret://m1/#f", "lake-secret:///n#f", "lake-secret://m1/n#", 3]) {
    assert.equal(parseLakeSecretRef(bad), null, String(bad));
  }
});

test("the table shows the secret and field, never a value", () => {
  assert.equal(secretEnvLabel("lake-secret://m1/database#DATABASE_URL"), "database → DATABASE_URL");
  assert.equal(secretEnvLabel("legacy-name"), "Secret");
});

test("the form sends a secret when a mount is given, else a plain value", () => {
  assert.deepEqual(envInput({ key: " DB ", mount_id: "m1", secret_name: "database", field: "DATABASE_URL", value: "ignored" }), {
    key: "DB",
    secret: { mount_id: "m1", name: "database", field: "DATABASE_URL" },
  });
  assert.deepEqual(envInput({ key: "LOG_LEVEL", value: "info" }), { key: "LOG_LEVEL", value: "info" });
  assert.deepEqual(envInput({ key: "EMPTY" }), { key: "EMPTY", value: "" });
});
