/** Load data → Upload names the dataset after the chosen file (lake#814). `npm test`; the build runs
 * it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { nameForFile, nextFreeExample } from "../src/pages/train/upload.ts";

test("choosing a file fills an empty Dataset name from the file's name, made valid", () => {
  assert.equal(nameForFile("", "", "customers.csv"), "customers");
  assert.equal(nameForFile("", "", "My Data 2024.csv"), "my_data_2024");
  assert.equal(nameForFile("", "", "events.jsonl"), "events");
  assert.equal(nameForFile("  ", "", "2026-q3.csv"), "_2026_q3", "a SQL name starts with a letter or underscore");
  assert.match(nameForFile("", "", "a".repeat(80) + ".csv"), /^[A-Za-z_][A-Za-z0-9_]{0,62}$/);
});

test("a name the person typed is kept when a file is chosen", () => {
  assert.equal(nameForFile("accounts", "", "customers.csv"), "accounts");
  assert.equal(nameForFile("accounts", "customers", "orders.csv"), "accounts", "edited after Lake filled it");
});

test("a name Lake filled in follows the next file chosen", () => {
  assert.equal(nameForFile("customers", "customers", "orders.csv"), "orders");
});

test("clearing the file leaves the name as it is", () => {
  assert.equal(nameForFile("customers", "customers", null), "customers");
});

test("the rename option's example follows the name", () => {
  assert.equal(nextFreeExample("customers"), "customers_2");
  assert.equal(nextFreeExample(""), "accounts_2");
});
