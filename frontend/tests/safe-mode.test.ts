/** A database's safe mode (lake#906, docs/ui/safe-mode.md): the levels and their words, the
 * warning read from an error, the confirmation added to the repeated call, and the database the
 * query editor's status bar shows. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import {
  DEFAULT_SAFE_MODE,
  SAFE_MODES,
  isSafeMode,
  queryDatabase,
  safeModeLabel,
  safeModeWarning,
  targetInput,
  targetName,
  warningSummary,
  withConfirmation,
} from "../src/lib/safeMode.ts";

test("three levels, Warn Writes by default, said in words", () => {
  assert.deepEqual(
    SAFE_MODES.map((m) => [m.mode, m.label]),
    [
      ["off", "Off"],
      ["warn_writes", "Warn Writes"],
      ["warn_all", "Warn All"],
    ],
  );
  assert.equal(DEFAULT_SAFE_MODE, "warn_writes");
  assert.equal(safeModeLabel("warn_all"), "Warn All");
  assert.equal(safeModeLabel("loud"), "Unknown");
  assert.equal(isSafeMode("off"), true);
  assert.equal(isSafeMode("Off"), false);
});

test("a level applies to a database or a connection, sent as the workspace takes it", () => {
  assert.deepEqual(targetInput({ database: "datasets" }), { database: "datasets" });
  assert.deepEqual(targetInput({ connection: "warehouse" }), { connection: "warehouse" });
  assert.equal(targetName({ connection: "warehouse" }), "warehouse");
});

test("a safe_mode_warning carries the statements, the target and the confirmation", () => {
  const error = {
    code: "safe_mode_warning",
    message: "safe mode warn_all",
    details: { connection: "warehouse", mode: "warn_all", statements: ["SELECT * FROM accounts", 7], confirmation: "9f3a1c" },
  };
  const warning = safeModeWarning(error);
  assert.ok(warning);
  assert.deepEqual(warning.target, { connection: "warehouse" });
  assert.equal(warning.mode, "warn_all");
  assert.deepEqual(warning.statements, ["SELECT * FROM accounts"], "only text is shown");
  assert.equal(warning.confirmation, "9f3a1c");
  assert.equal(warningSummary(warning), "Warn All is on for the connection warehouse. Check the query before it runs.");
  const two = safeModeWarning({ code: "safe_mode_warning", details: { database: "datasets", mode: "warn_writes", statements: ["DELETE FROM a", "DELETE FROM b"], confirmation: "aa" } });
  assert.ok(two);
  assert.equal(warningSummary(two), "Warn Writes is on for the database datasets. Check the 2 queries before they run.");
});

test("any other error, or a warning without a confirmation, is not a warning", () => {
  assert.equal(safeModeWarning({ code: "engine_error", details: { confirmation: "aa" } }), null);
  assert.equal(safeModeWarning({ code: "safe_mode_warning", details: {} }), null, "nothing to repeat the call with");
  assert.equal(safeModeWarning(null), null);
  assert.equal(safeModeWarning(new Error("x")), null);
});

test("the repeated call carries the confirmation, and only then", () => {
  assert.deepEqual(withConfirmation({ connection: "warehouse" }, "9f3a1c"), { connection: "warehouse", safe_mode_confirmation: "9f3a1c" });
  assert.deepEqual(withConfirmation({ connection: "warehouse" }, undefined), { connection: "warehouse" });
  assert.deepEqual(withConfirmation({ connection: "warehouse" }, ""), { connection: "warehouse" });
});

test("the query editor's database: the field, or the database the statement names", () => {
  assert.equal(queryDatabase("datasets", "SELECT 1"), "datasets");
  assert.equal(queryDatabase(" ", "SELECT plan FROM datasets.accounts LIMIT 5"), "datasets");
  assert.equal(queryDatabase("", 'select * from "sales"."orders"'), "sales");
  assert.equal(queryDatabase("", "SELECT * FROM accounts"), "", "an unqualified table names no database");
});

test("the query editor and each connection show the control, and a warning opens the dialog", () => {
  const read = (path: string) => readFileSync(join(import.meta.dirname, "..", "src", path), "utf8");
  const query = read("pages/train/QueryPage.tsx");
  assert.match(query, /<SafeModeControl variant="status"/);
  assert.match(query, /useSafeModeRead\(/);
  assert.match(query, /safe_mode_confirmation/);
  const connection = read("pages/connections/ConnectionPage.tsx");
  assert.match(connection, /<SafeModeControl target=\{\{ connection: name \}\}/);
  assert.match(connection, /useSafeModeRead\(tables\.error/);
  assert.match(read("pages/connections/ConnectionsPage.tsx"), /<SafeModeControl variant="status" target=\{\{ connection:/);
  assert.match(read("pages/connections/forms.tsx"), /safe\.run\(/);
  const control = read("pages/connections/SafeMode.tsx");
  assert.match(control, /role="menuitemradio"/);
  assert.match(control, /aria-checked=\{option\.mode === mode\}/);
  assert.match(control, />\s*Run\s*</);
  assert.match(control, /<Button onClick=\{onCancel\}>Cancel<\/Button>/);
});
