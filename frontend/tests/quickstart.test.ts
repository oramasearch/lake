// SPDX-License-Identifier: MIT

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
  agentPrompt,
  modelView,
  showsTraining,
  isClustering,
  elapsed,
  failureReason,
  fixMessage,
  modelStepTitle,
  opensConversation,
  whereItLives,
  DEFAULT_WEBSITE_VIEW,
  WEBSITE_VIEWS,
  type Column,
} from "../src/pages/quickstart/quickstart.ts";

// The reported dataset (lake#1181) as quickstart_status answers it: every column with its detected kind.
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

test("the website step opens on the agent prompt, with the source code beside it (lake#1185)", () => {
  assert.equal(DEFAULT_WEBSITE_VIEW, "prompt");
  assert.deepEqual(WEBSITE_VIEWS.map((v) => v.label), ["Copy agent prompt", "Source code"]);
});

test("the agent prompt carries the key only where the page holds it", () => {
  const key = { kind: "browser_key" as const, placeholder: "YOUR_LAKE_SEARCH_KEY", capabilities: ["search_index"], create: { capability: "create_browser_key", input: {} }, secret: null, browser_key: null, note: "" };
  const snippets = { prompt: "Authorization: Bearer YOUR_LAKE_SEARCH_KEY ... Bearer YOUR_LAKE_SEARCH_KEY", key };
  assert.equal(agentPrompt(snippets, null), snippets.prompt, "no key: it says how to make one");
  assert.equal(agentPrompt(snippets, "lbk.abc"), "Authorization: Bearer lbk.abc ... Bearer lbk.abc");
  assert.equal(agentPrompt({ key }, "lbk.abc"), null, "a Lake that answers no prompt");
  assert.equal(agentPrompt(null, null), null);
});

// lake#1182: step 4 is an Ask Orama conversation, opened once search works.
test("the model step opens its conversation once search works and nothing is training", () => {
  const ready = { quickstart_id: "q1", index: { id: "i", fields: [], published: true }, conversation: null, training: null, model: null };
  assert.equal(opensConversation(ready, false), true);
  assert.equal(opensConversation(ready, true), false, "a blocked balance offers the top-up first");
  assert.equal(opensConversation({ ...ready, index: { id: "i", fields: [], published: false } }, false), false);
  assert.equal(opensConversation({ ...ready, conversation: { conversation_id: "c", task_id: null, page: {} } }, false), false, "once");
  assert.equal(opensConversation({ ...ready, training: { target: "x" } }, false), false);
  assert.equal(opensConversation(null, false), false);
  assert.equal(modelStepTitle({ training: null, model: null }), "Decide What to Build");
  assert.equal(modelStepTitle({ training: { target: "x" }, model: null }), "Train a Model on the Same Data");
});

test("a confirmed proposal links where what it made lives, and a model goes on in the page", () => {
  assert.equal(whereItLives("train_quickstart"), null);
  assert.deepEqual(whereItLives("create_app"), { path: "/ship/apps", label: "Ship → Apps" });
  assert.deepEqual(whereItLives("generate_dataset"), { path: "/train/data", label: "Data → Datasets" });
  assert.deepEqual(whereItLives("create_projects_from_opportunities"), { path: "/design/opportunities", label: "Design → Opportunities" });
  assert.deepEqual(whereItLives("compile_intent"), { path: "/design/projects", label: "Design → Projects" });
  assert.equal(whereItLives("set_app_env"), null);
});

test("a skipped step reads as neutral", () => {
  assert.equal(stepTone("skipped"), "neutral");
});

// lake#1204: after the conversation's Confirm, the model step moves on by itself, and every state
// has one next action.
const steps = (model: string, answers = "pending", error: { code: string; message: string } | null = null) =>
  [
    { step: "workspace", title: "Workspace", state: "done" as const, error: null },
    { step: "model", title: "Model", state: model as "pending", error: model === "failed" ? error : null },
    { step: "model_answers", title: "Model Answers on Your Website", state: answers as "pending", error: null },
  ];

test("confirming a model moves the step to its training at once, and each state is named", () => {
  const nothing = { state: "search_ready" as const, steps: steps("pending"), training: null, model: null };
  assert.equal(modelView(nothing), "choose");
  assert.equal(showsTraining(nothing), false, "the conversation");
  const confirmed = { state: "training" as const, steps: steps("running"), training: { run_id: "r", methodology: "M-3" }, model: null };
  assert.equal(modelView(confirmed), "training");
  assert.equal(showsTraining(confirmed), true);
  // Refused before a run started (lake#1203 on dev): the training view with why, not the conversation.
  const refused = { state: "failed" as const, steps: steps("failed", "pending", { code: "invalid_input", message: "Lake could not train the clustering model." }), training: { run_id: null, methodology: "M-3" }, model: null };
  assert.equal(modelView(refused), "failed");
  assert.equal(showsTraining(refused), true);
  assert.equal(showsTraining({ ...refused, training: null }), true, "a failed model step alone moves it");
  assert.equal(modelView({ state: "trained" as const, steps: steps("done"), training: { run_id: "r" }, model: { id: "m", published: false } }), "publishing");
  assert.equal(modelView({ state: "done" as const, steps: steps("done", "done"), training: { run_id: "r" }, model: { id: "m", published: true } }), "done");
  assert.equal(modelView({ state: "done" as const, steps: steps("done", "skipped"), training: { run_id: "r", methodology: "M-3" }, model: { id: "m", published: false, serves: false } }), "clustered");
});

test("a failure is said in the step's plain words, and Ask Orama is asked for the corrected change", () => {
  const failed = { steps: steps("failed", "pending", { code: "invalid_input", message: "Clustering needs at least 2 rows." }), training: { methodology: "M-3" } };
  assert.equal(failureReason(failed), "Clustering needs at least 2 rows.");
  assert.match(failureReason({ steps: steps("failed") }), /Try again/);
  const said = fixMessage(failed);
  assert.match(said, /^The clustering model did not train: Clustering needs at least 2 rows\./);
  assert.match(said, /Propose the corrected change/);
  assert.doesNotMatch(said, /configure|train_quickstart|K range/);
  assert.match(fixMessage({ steps: failed.steps, training: { target: "churned" } }), /^The model that predicts churned/);
  assert.equal(isClustering("M-3"), true);
  assert.equal(isClustering("segmentation"), true);
  assert.equal(isClustering("M-1"), false);
  assert.equal(isClustering(null), false);
});

test("training says how long it has run", () => {
  const start = Date.parse("2026-10-25T21:20:00Z");
  assert.equal(elapsed("2026-10-25T21:20:00Z", start + 20_000), "under a minute");
  assert.equal(elapsed("2026-10-25T21:20:00Z", start + 5 * 60_000), "5 min");
  assert.equal(elapsed("2026-10-25T21:20:00Z", start + 125 * 60_000), "2 h 5 min");
  assert.equal(elapsed("", start), "");
  assert.equal(elapsed(undefined, start), "");
});
