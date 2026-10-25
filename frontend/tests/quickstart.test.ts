/** The quickstart process's page rules (lake#1128, docs/ui/quickstart.md): what a dropped file is
 * sent as, the first model's columns, polling, publishing a trained model, the website key and the
 * balance. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  MAX_UPLOAD_BYTES,
  blockedBy,
  columnUses,
  dateInput,
  detected,
  keyCovers,
  currentStep,
  fileKind,
  firstColumns,
  hitLine,
  kindLabel,
  pollInterval,
  predictable,
  publishesNow,
  rateLabel,
  siteOrigin,
  startingChoice,
  stepTone,
  trainInput,
  trainLabel,
  uploadContent,
  valuesOf,
  withKey,
  type Column,
} from "../src/pages/quickstart/quickstart.ts";

// Steve's dataset (lake#1181) as quickstart_status answers it: every column with its detected kind.
const people: Column[] = [
  { name: "customer_id", type: "string", kind: "identifier", title: "identifier", feature: { use: false, reason: "an identifier: it names a row" }, predict: { can: false, reason: "an identifier" } },
  { name: "First Name", type: "string", kind: "categorical", title: "category, 10 values", feature: { use: true, reason: "a category of 10 values" }, predict: { can: true, choose_value: true, values: ["Ana", "Ben"] } },
  { name: "Country", type: "string", kind: "categorical", title: "category, 4 values", feature: { use: true, reason: "a category of 4 values" }, predict: { can: true, choose_value: true, values: ["Canada", "Japan"] } },
  { name: "Signup Date", type: "string", kind: "datetime", title: "date or time", feature: { use: false, reason: "a date or time" }, predict: { can: false, reason: "a date" } },
  { name: "Plan", type: "string", kind: "boolean", title: "yes or no", feature: { use: true, reason: "two values" }, predict: { can: true, values: ["free", "pro"], positive: "pro" } },
  { name: "Spend", type: "number", kind: "numeric", title: "number", feature: { use: true, reason: "a number" }, predict: { can: false, reason: "a number with many values" } },
];
const dataset = { id: "datasets/people", database: "datasets", name: "people", columns: people };

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

test("the website's address becomes the origin its browser key lists (lake#1152)", () => {
  assert.deepEqual(siteOrigin("https://www.example.com"), { origin: "https://www.example.com" });
  assert.deepEqual(siteOrigin("  WWW.Example.com/products?x=1 "), { origin: "https://www.example.com" });
  assert.deepEqual(siteOrigin("https://shop.example.com:8443/"), { origin: "https://shop.example.com:8443" });
  assert.deepEqual(siteOrigin("https://shop.example.com:443"), { origin: "https://shop.example.com" });
  assert.deepEqual(siteOrigin("localhost:5173"), { origin: "http://localhost:5173" });
  assert.deepEqual(siteOrigin("http://127.0.0.1:8080/app"), { origin: "http://127.0.0.1:8080" });
  assert.match((siteOrigin("") as { error: string }).error, /address/);
  assert.match((siteOrigin("http://example.com") as { error: string }).error, /https/);
  assert.match((siteOrigin("ftp://example.com") as { error: string }).error, /https/);
  assert.match((siteOrigin("https://exa mple.com") as { error: string }).error, /not a website/);
});

test("the column to predict lists only predictable columns, each with its detected type (lake#1181)", () => {
  assert.equal(detected(people), true);
  assert.equal(detected([{ name: "a", type: "string" }]), false, "an engine that could not profile it");
  assert.deepEqual(predictable(people), [
    { name: "First Name", label: "First Name (category, 10 values)" },
    { name: "Country", label: "Country (category, 4 values)" },
    { name: "Plan", label: "Plan (yes or no)" },
  ]);
  assert.equal(kindLabel(people[3]), "date or time");
  assert.deepEqual(valuesOf(people, "Country"), ["Canada", "Japan"], "a category asks which value");
  assert.deepEqual(valuesOf(people, "Plan"), [], "two values need no choice");
});

test("the page starts from what Lake suggests, then from what the last training used", () => {
  const fresh = startingChoice({ dataset, index: null, training: null, snapshot_date: "2026-10-24T08:00:00Z", suggested: { target: "Country", target_value: "Canada", entity_column: "customer_id" } });
  assert.deepEqual(fresh, { target: "Country", value: "Canada", entity: "customer_id", asOf: "2026-10-24" });
  const again = startingChoice({ dataset, index: null, training: { target: "Country", target_value: "Japan", entity_column: "First Name", as_of: "2026-01-31" }, snapshot_date: "2026-01-31", suggested: { target: "Plan", target_value: null, entity_column: "customer_id" } });
  assert.deepEqual(again, { target: "Country", value: "Japan", entity: "First Name", asOf: "2026-01-31" });
  // Without a profile, the file's order as before.
  const plain = startingChoice({ dataset: { ...dataset, columns: [{ name: "title", type: "string" }, { name: "popular", type: "integer" }] }, index: null, training: null, suggested: null, snapshot_date: null });
  assert.deepEqual(plain, { target: "popular", value: "", entity: "title", asOf: "" });
});

test("every column says what happens to it, left-out ones with why", () => {
  const uses = columnUses(people, { target: "Country", entity: "customer_id" });
  assert.deepEqual(
    uses.map((u) => [u.name, u.use]),
    [
      ["customer_id", "Names a row"],
      ["First Name", "Learned from"],
      ["Country", "Predicted"],
      ["Signup Date", "Left out"],
      ["Plan", "Learned from"],
      ["Spend", "Learned from"],
    ],
  );
  assert.equal(uses[3].reason, "a date or time");
  assert.equal(uses[3].used, false);
});

test("training sends the choices, and the snapshot date only when it was changed", () => {
  const choice = { target: "Country", value: "Japan", entity: "First Name", asOf: "2026-10-24" };
  assert.deepEqual(trainInput(choice, people, "2026-10-24T08:00:00Z"), { target: "Country", target_value: "Japan", entity_column: "First Name" });
  assert.deepEqual(trainInput({ ...choice, asOf: "2026-01-31" }, people, "2026-10-24T08:00:00Z"), { target: "Country", target_value: "Japan", entity_column: "First Name", as_of: "2026-01-31" });
  assert.deepEqual(trainInput({ target: "Plan", value: "x", entity: "", asOf: "" }, people, null), { target: "Plan" }, "no value for two values, no row name");
  assert.equal(dateInput("2026-10-24T08:00:00Z"), "2026-10-24");
  assert.equal(dateInput("soon"), "");
});

test("after a failure the button trains with the changed choices, never 'Try again'", () => {
  assert.equal(trainLabel(false), "Train a model");
  assert.equal(trainLabel(true), "Train with these choices");
});
