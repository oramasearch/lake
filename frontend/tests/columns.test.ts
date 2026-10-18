/** A table's columns follow the order the answer names, not the rows' sorted keys (lake#812): the
 * Query page's results, and the dataset page's Columns and First Rows tabs. `npm test`; the build
 * runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { datasetColumns, tableColumns } from "../src/pages/train/columns.ts";

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
