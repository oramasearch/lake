/** The quickstart process's page rules (lake#1128, docs/ui/quickstart.md): what a dropped file is
 * sent as, the first model's columns, polling, publishing a trained model, the website key and the
 * balance. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { MAX_UPLOAD_BYTES, blockedBy, keyCovers, currentStep, fileKind, firstColumns, hitLine, pollInterval, publishesNow, rateLabel, stepTone, uploadContent, withKey } from "../src/pages/quickstart/quickstart.ts";

test("a dropped file is sent as what upload_dataset takes", () => {
  assert.deepEqual(uploadContent("movies.csv", "title,genre\nAlien,horror\n"), { content: "title,genre\nAlien,horror\n", format: "csv" });
  assert.deepEqual(uploadContent("events.JSONL", '{"a":1}\n'), { content: '{"a":1}\n', format: "jsonl" });
  assert.deepEqual(uploadContent("events.ndjson", '{"a":1}\n'), { content: '{"a":1}\n', format: "jsonl" });
  // A JSON array of objects becomes one line per object; one object is one row.
  assert.deepEqual(uploadContent("rows.json", '[{"a":1},{"a":2,"b":"x"}]'), { content: '{"a":1}\n{"a":2,"b":"x"}\n', format: "jsonl" });
  assert.deepEqual(uploadContent("one.json", '{"a":1}'), { content: '{"a":1}\n', format: "jsonl" });
});

test("a file it cannot send says why", () => {
  assert.match((uploadContent("data.parquet", "PAR1") as { error: string }).error, /Parquet/);
  assert.match((uploadContent("notes.txt", "x") as { error: string }).error, /CSV, JSON or JSON Lines/);
  assert.match((uploadContent("empty.csv", "  \n") as { error: string }).error, /empty/);
  assert.match((uploadContent("bad.json", "{oops") as { error: string }).error, /not valid JSON/);
  assert.match((uploadContent("list.json", "[1,2]") as { error: string }).error, /object/);
  assert.match((uploadContent("none.json", "[]") as { error: string }).error, /empty/);
  assert.deepEqual(fileKind("A.CSV"), { kind: "csv" });
  assert.equal(MAX_UPLOAD_BYTES, 16 * 1024 * 1024, "upload_dataset's limit");
});

test("the first model predicts the last column and names rows by the first", () => {
  assert.deepEqual(firstColumns(["title", "genre", "rating", "popular"]), { target: "popular", entity: "title" });
  assert.deepEqual(firstColumns(["only"]), { target: "only", entity: "" });
  assert.deepEqual(firstColumns([]), { target: "", entity: "" });
  // The file's own order and types, not the index's sorted fields.
  const columns = [
    { name: "rating", type: "number" },
    { name: "title", type: "string" },
    { name: "genre", type: "string" },
    { name: "popular", type: "integer" },
  ];
  assert.deepEqual(firstColumns(["genre", "popular", "rating", "title"], columns), { target: "popular", entity: "title" });
});

test("the page reads the status often while the workspace starts or the model trains, and not otherwise", () => {
  assert.equal(pollInterval("waiting_for_workspace"), 5000);
  assert.equal(pollInterval("training"), 3000);
  for (const state of ["not_started", "search_ready", "trained", "done", "failed", undefined] as const) {
    assert.equal(pollInterval(state), false, String(state));
  }
});

test("a trained model is published by the click that trained it", () => {
  assert.equal(publishesNow({ state: "trained" }), true);
  assert.equal(publishesNow({ state: "training" }), false);
  assert.equal(publishesNow({ state: "done" }), false);
  assert.equal(publishesNow(null), false);
});

test("the current step is the first not done", () => {
  const step = (name: string, state: "done" | "pending" | "running" | "failed") => ({ step: name, title: name, state, error: null });
  assert.equal(currentStep([step("workspace", "done"), step("dataset", "failed"), step("search_index", "pending")]), "dataset");
  assert.equal(currentStep([step("workspace", "done")]), "done");
  assert.equal(stepTone("done"), "success");
  assert.equal(stepTone("running"), "info");
  assert.equal(stepTone("failed"), "danger");
  assert.equal(stepTone("pending"), "neutral");
});

test("the website code carries the key once there is one, everywhere it says the placeholder", () => {
  const code = 'const KEY = "YOUR_LAKE_SEARCH_KEY";\n// YOUR_LAKE_SEARCH_KEY again';
  assert.equal(withKey(code, "YOUR_LAKE_SEARCH_KEY", null), code);
  assert.equal(withKey(code, "YOUR_LAKE_SEARCH_KEY", "lk_secret"), 'const KEY = "lk_secret";\n// lk_secret again');
});

test("a blocked balance offers the top-up link instead of the paid step", () => {
  assert.equal(blockedBy({ state: "blocked" }), true);
  assert.equal(blockedBy({ state: "blocked", exempt: "paid_order" }), false);
  assert.equal(blockedBy({ state: "low" }), false);
  assert.equal(blockedBy(null), false);
});

test("rates and hits read as words", () => {
  const usd = (micros: number) => `$${(micros / 1_000_000).toFixed(3)}`;
  assert.equal(rateLabel({ unit_micros: 2000, per: 60000 }, "mcpu_seconds", usd), "$0.002 per 60,000 mcpu seconds");
  assert.equal(rateLabel({ unit_micros: 500000, per: 1 }, "calls", usd), "$0.500 per call");
  assert.equal(rateLabel(null, "calls", usd), null);
  assert.equal(hitLine({ title: "Alien", genre: "horror", rating: 8.5, none: null }), "Alien · horror · 8.5");
  assert.equal(hitLine(undefined), "");
});

test("a key made before the model was published is not enough for its answers", () => {
  assert.equal(keyCovers(["search_index"], ["search_index"]), true);
  assert.equal(keyCovers(["search_index"], ["search_index", "predict"]), false);
  assert.equal(keyCovers(["search_index", "predict"], ["search_index", "predict"]), true);
});
