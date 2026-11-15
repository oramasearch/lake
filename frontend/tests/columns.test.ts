// SPDX-License-Identifier: MIT

/** A table's columns follow the order the answer names, not the rows' sorted keys (lake#812): the
 * Query page's results, and the dataset page's Columns and First Rows tabs. `npm test`; the build
 * runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { cappedStatement, columnFacts, datasetColumns, firstRows, limitRefused, tableColumns, typeLabel } from "../src/pages/train/columns.ts";

// The answer lake-dev gave the review's query (lake#812): `columns` in SELECT order, each row's keys
// sorted.
const answer = {
  columns: ["plan", "tenure_months", "churned"],
  rows: [
    { churned: 0, plan: "basic", tenure_months: 21 },
    { churned: 1, plan: "pro", tenure_months: 3 },
  ],
};

test("a query whose SELECT order is not alphabetical keeps that order in its headers", () => {
  const headers = tableColumns(answer.rows, answer.columns);
  assert.deepEqual(headers, ["plan", "tenure_months", "churned"]);
  assert.notDeepEqual(headers, [...headers].sort(), "the SELECT order is not alphabetical");
  // The cells follow the headers.
  assert.deepEqual(
    answer.rows.map((row) => headers.map((c) => (row as Record<string, unknown>)[c])),
    [
      ["basic", 21, 0],
      ["pro", 3, 1],
    ],
  );
});

test("an aggregate's alias order is kept", () => {
  const rows = [{ churn_rate: 0.2, n: 10, plan: "basic" }];
  assert.deepEqual(tableColumns(rows, ["plan", "churn_rate", "n"]), ["plan", "churn_rate", "n"]);
});

test("the columns are kept for no rows, and a key no column names comes last", () => {
  assert.deepEqual(tableColumns([], ["plan", "churned"]), ["plan", "churned"]);
  assert.deepEqual(tableColumns([{ a: 1, extra: 2, z: 3 }], ["z", "a"]), ["z", "a", "extra"]);
});

test("without named columns, the keys in the order the rows first carry them", () => {
  assert.deepEqual(tableColumns([{ b: 1 }, { a: 2, b: 3 }]), ["b", "a"]);
  assert.deepEqual(tableColumns([{ b: 1 }], "not a list"), ["b"]);
  assert.deepEqual(tableColumns(["text", null, [1]]), []);
});

test("a dataset's columns are the table's own order, never sorted", () => {
  const listed = ["customer_id", "plan", "tenure_months", "monthly_spend", "churned"];
  assert.deepEqual(datasetColumns(listed, undefined, undefined), listed);
  // The first rows' answer and the analysis only add names the dataset does not list.
  assert.deepEqual(datasetColumns(["plan", "churned"], ["plan", "region", "churned"], [{ name: "score" }, { name: "plan" }]), [
    "plan",
    "churned",
    "region",
    "score",
  ]);
  assert.deepEqual(datasetColumns(undefined, ["z", "a"], undefined), ["z", "a"]);
});

// zetta#1021: get_dataset answers typed columns in the file's order; an older engine answers names.
const typed = [
  { name: "account_id", type: "integer", nullable: false, null_fraction: 0 },
  { name: "plan", type: "string", nullable: true, null_fraction: 0.25 },
  { name: "monthly_spend", type: "number", nullable: false, null_fraction: 0 },
  { name: "churned", type: "boolean", nullable: false, null_fraction: null },
];

test("get_dataset's typed columns keep the file's order, for the Columns tab and the First Rows headers", () => {
  assert.deepEqual(datasetColumns(typed, ["churned", "plan"], []), ["account_id", "plan", "monthly_spend", "churned"]);
  const rows = [{ account_id: 1, churned: false, monthly_spend: 9.5, plan: "pro" }];
  assert.deepEqual(tableColumns(rows, typed), ["account_id", "plan", "monthly_spend", "churned"]);
});

test("get_dataset's types and null shares are read by name; names alone carry none", () => {
  const facts = columnFacts(typed);
  assert.deepEqual(facts.get("plan"), { type: "string", nullable: true, nullFraction: 0.25 });
  assert.equal(facts.get("churned")?.nullFraction, null, "an unknown share stays unknown");
  assert.equal(columnFacts(["account_id", "plan"]).size, 0, "an engine before zetta#1021 answers names");
  assert.deepEqual(
    ["integer", "number", "string", "boolean", "object", "array", "null", "mixed", "unknown"].map(typeLabel),
    ["integer", "decimal", "text", "boolean", "JSON", "JSON", "empty", "mixed", "unknown"],
  );
});

test("a column the caller may not read keeps its place but carries no facts", () => {
  // zetta#1299 answers a column hidden by a column-scoped sql:read with its name and the rest null.
  const masked = [...typed.slice(0, 2), { name: "ssn", type: null, nullable: null, null_fraction: null }, typed[3]];
  assert.deepEqual(datasetColumns(masked, [], []), ["account_id", "plan", "ssn", "churned"]);
  const facts = columnFacts(masked);
  assert.equal(facts.has("ssn"), false, "no type or share is shown for it");
  assert.equal(facts.get("plan")?.type, "string");
});

test("an engine that refuses LIMIT is told apart from a query's own error", () => {
  assert.ok(limitRefused(new Error("expected ';' after statement, got 'LIMIT' (at position 30)")));
  assert.ok(limitRefused({ message: "expected ';' after statement, got 'limit'" }));
  assert.ok(!limitRefused(new Error("column \"nope\" does not exist in datasets.accounts")));
  assert.ok(!limitRefused(undefined));
  assert.deepEqual(firstRows([1, 2, 3, 4], 2), [1, 2], "the fallback keeps the first rows itself");
  assert.deepEqual(firstRows(undefined, 2), []);
});

test("the Query page asks for one row more than it shows, unless the statement pages itself", () => {
  assert.equal(cappedStatement("SELECT * FROM datasets.accounts", 500), "SELECT * FROM datasets.accounts LIMIT 501");
  assert.equal(cappedStatement("  select plan from t;  ", 500), "select plan from t LIMIT 501");
  for (const own of ["SELECT * FROM t LIMIT 5", "SELECT * FROM t OFFSET 3", "SELECT * FROM t FETCH FIRST 2 ROWS ONLY", "WITH x AS (SELECT 1) SELECT * FROM x", "SELECT 1; SELECT 2"]) {
    assert.equal(cappedStatement(own, 500), own, own);
  }
  assert.equal(cappedStatement("SELECT * FROM t WHERE note = 'no limit here'", 500), "SELECT * FROM t WHERE note = 'no limit here' LIMIT 501", "quoted text is not a LIMIT");
  assert.equal(cappedStatement("SELECT * FROM t -- every account", 500), "SELECT * FROM t -- every account\nLIMIT 501", "a line comment does not swallow the LIMIT");
});
