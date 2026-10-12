/**
 * Train → Models and Ship → Models (lake#599): which trained records are versions of one model,
 * their held-out scores and comparisons, the pipeline steps a model belongs to, the predict call
 * and the try-it rows, the keys that may call predict, and a model's traffic.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  broughtFrom,
  compareVersions,
  errorRate,
  featuresOf,
  filledSteps,
  groupModels,
  groupOf,
  heldOutScore,
  holdoutOf,
  isBrought,
  measuredFigures,
  methodologyOf,
  modelSteps,
  nameProblem,
  needsTarget,
  parseRows,
  predictCurl,
  predictKeys,
  predictionRows,
  sampleRow,
  scoreSeries,
  serverState,
  trafficOf,
  trainingOf,
  versionNumber,
} from "../src/pages/train/models.ts";

const churn = (id: string, created: string, auc: number | null, extra: Record<string, unknown> = {}) => ({
  id,
  name: "churn",
  kind: "classifier",
  project: "p-1",
  project_name: "Retention",
  pipeline_name: "weekly",
  run: `run-${id}`,
  created,
  metrics: auc === null ? { in_sample_auc: 0.9 } : { held_out_auc: auc, in_sample_auc: 0.95, held_out_rows: 200 },
  feature_schema: { features: ["tenure", "tickets"] },
  exposed: false,
  ...extra,
});

const records = [
  churn("a", "2026-10-01T10:00:00Z", 0.71),
  churn("b", "2026-10-03T10:00:00Z", 0.74, { exposed: true }),
  churn("c", "2026-10-05T10:00:00Z", null),
  // The same pipeline's other model step: another model.
  { ...churn("d", "2026-10-02T10:00:00Z", 0.6), name: "upsell" },
  // A trial of an experiment with no pipeline, and one trained by nothing named.
  { id: "e", name: "gbdt-binary-logistic", kind: "classifier", project: "", pipeline_name: "", created: "2026-10-04T10:00:00Z", metrics: { held_out_rmse: 3 } },
  { id: "f", name: "gbdt-binary-logistic", kind: "classifier", project: "", pipeline_name: "", created: "2026-10-06T10:00:00Z", metrics: {} },
];

test("records of one pipeline's model step are versions of one model, newest first", () => {
  const groups = groupModels(records, (model) => (model.id === "e" ? { id: "x-1", name: "Tuning" } : undefined));
  assert.equal(groups.length, 4);
  const model = groupOf(groups, "a")!;
  assert.deepEqual(model.versions.map((v) => v.id), ["c", "b", "a"]);
  assert.equal(model.name, "churn");
  assert.equal(model.pipeline, "weekly");
  assert.deepEqual(model.published.map((v) => v.id), ["b"]);
  assert.deepEqual(model.runs, ["run-c", "run-b", "run-a"]);
  // The latest version measured nothing held out: the newest score is shown.
  assert.equal(model.score?.value, 0.74);
  assert.equal(versionNumber(model, "c"), 3);
  assert.equal(versionNumber(model, "a"), 1);
  assert.notEqual(groupOf(groups, "d")!.key, model.key, "a pipeline may hold several models");
  assert.notEqual(groupOf(groups, "e")!.key, groupOf(groups, "f")!.key, "an experiment's trial is its own model");
  assert.equal(groups[0].latest.id, "f", "the newest model first");
});

test("a held-out score is never an in-sample figure", () => {
  assert.equal(heldOutScore({ metrics: { in_sample_auc: 0.9 } }), null);
  assert.deepEqual(heldOutScore({ metrics: { held_out_rmse: 2.5 } }), { key: "held_out_rmse", label: "RMSE", value: 2.5, higher: false });
  assert.equal(heldOutScore({ metrics: { holdout_auc: 0.8 } })?.label, "AUC");
});

test("the score over versions is oldest first and skips versions that did not measure it", () => {
  const model = groupOf(groupModels(records), "a")!;
  assert.deepEqual(scoreSeries(model, "held_out_auc").map((p) => p.id), ["a", "b"]);
  assert.deepEqual(measuredFigures(model).map((f) => f.label), ["AUC"]);
});

test("comparing versions says which held-out figures got better", () => {
  const changes = compareVersions(records[0], records[1]);
  assert.equal(changes[0].key, "held_out_auc");
  assert.equal(changes[0].better, true);
  assert.equal(changes[0].from, 0.71);
  assert.equal(changes[0].to, 0.74);
  const rmse = compareVersions({ metrics: { held_out_rmse: 2 } }, { metrics: { held_out_rmse: 3 } });
  assert.equal(rmse[0].better, false, "a larger error is worse");
  const other = changes.find((c) => c.key === "in_sample_auc")!;
  assert.equal(other.better, null, "an in-sample figure is not judged");
});

test("a model's pipeline steps: the step named as the model, else every model step", () => {
  const graph = {
    nodes: [
      { id: "s1", kind: "connect", name: "load" },
      { id: "s2", kind: "model", name: "churn" },
      { id: "s3", kind: "model", name: "upsell" },
    ],
  };
  assert.deepEqual(modelSteps(graph, "churn"), { steps: [{ id: "s2", kind: "model", name: "churn", attributes: {} }], exact: true });
  const unnamed = modelSteps(graph, "gbdt-binary-logistic");
  assert.equal(unnamed.steps.length, 2);
  assert.equal(unnamed.exact, false);
  assert.equal(modelSteps({ nodes: [{ id: "m", kind: "model", name: "train", attributes: { model_name: "churn" } }] }, "churn").exact, true);
  assert.deepEqual(modelSteps(null, "churn"), { steps: [], exact: false });
});

test("the predict call carries the account, workspace, model and a sample row", () => {
  assert.deepEqual(sampleRow(records[0]), { entity: "example-1", tenure: 0, tickets: 0 });
  const curl = predictCurl("https://lake.example", "cust", "ws", records[0]);
  assert.match(curl, /^curl -s https:\/\/lake\.example\/v1\/predict/);
  assert.match(curl, /"model_id":"a"/);
  assert.match(curl, /"workspace_id":"ws"/);
  assert.match(curl, /"tenure":0/);
  assert.match(predictCurl("o", "c", "", records[0]), /<workspace id>/);
});

test("try-it rows are one JSON object or a list of them", () => {
  assert.deepEqual(parseRows('{"tenure": 3}'), { rows: [{ tenure: 3 }] });
  assert.deepEqual(parseRows('[{"a": 1}, {"a": 2}]'), { rows: [{ a: 1 }, { a: 2 }] });
  assert.ok("error" in parseRows("tenure=3"));
  assert.ok("error" in parseRows("[]"));
  assert.ok("error" in parseRows("[1, 2]"));
});

test("predict keys are unrevoked tokens scoped to predict or to everything", () => {
  const keys = predictKeys([
    { id: "1", kind: "api_token", scopes: ["predict"] },
    { id: "2", kind: "api_token", scopes: ["predict"], revoked_at: "2026-10-01" },
    { id: "3", kind: "static", scopes: ["*"] },
    { id: "4", kind: "api_token", scopes: ["search_index"] },
    { id: "5", kind: "browser", scopes: ["predict"] },
  ]);
  assert.deepEqual(keys.map((k) => k.id), ["1", "3"]);
});

test("a model's traffic, its error rate and its quiet steps", () => {
  const answer = {
    models: [{ model_id: "b", calls: 40, errors: 1, client_errors: 1, server_errors: 0, mean_ms: 12.5, steps: [{ at_ms: 120_000, calls: 40, errors: 1, mean_ms: 12.5 }] }],
  };
  const traffic = trafficOf(answer, "b");
  assert.equal(traffic.calls, 40);
  assert.equal(traffic.meanMs, 12.5);
  assert.equal(errorRate(traffic), "2.5%");
  assert.equal(trafficOf(answer, "a").calls, 0, "a model with no calls");
  assert.equal(errorRate(trafficOf(answer, "a")), "—");
  const steps = filledSteps(traffic, 0, 240_000, 60_000);
  assert.deepEqual(steps.map((s) => s.calls), [0, 0, 40, 0]);
});

test("a model's features read by name whatever key the trainer used, never as JSON", () => {
  const model = { feature_schema: { features: ["tenure", { name: "plan" }, { encoded_name: "monthly_value", kind: "float", column: null }, { column: "region" }, { kind: "float" }] } };
  assert.deepEqual(featuresOf(model), ["tenure", "plan", "monthly_value", "region"]);
});

/* ------------------------------------------------------------------ standalone models (lake#654) */

const standalone = (id: string, created: string, extra: Record<string, unknown> = {}) => ({
  id,
  name: "churn",
  kind: "classifier",
  project: "p-internal",
  project_name: "churn (standalone model)",
  pipeline_name: "churn",
  run: `run-${id}`,
  created,
  metrics: { held_out_auc: 0.8 },
  feature_schema: { features: ["tenure"] },
  standalone: true,
  ...extra,
});

test("a standalone model's records are its versions, and it says it is standalone", () => {
  const groups = groupModels([...records, standalone("s1", "2026-10-07T10:00:00Z"), standalone("s2", "2026-10-08T10:00:00Z")]);
  const model = groupOf(groups, "s1")!;
  assert.deepEqual(model.versions.map((v) => v.id), ["s2", "s1"]);
  assert.equal(model.standalone, true);
  assert.equal(model.brought, false);
  // A project's churn model of the same name is another model.
  assert.notEqual(groupOf(groups, "a")!.key, model.key);
  assert.equal(groupOf(groups, "a")!.standalone, false);
});

test("a brought model groups by its project and says where it came from", () => {
  const brought = (id: string, created: string) =>
    standalone(id, created, { name: "partner", pipeline_name: "", run: "", project: "p-b", feature_schema: { features: ["a"], brought: true, source: "drive Docs: partner.json" } });
  const groups = groupModels([brought("b1", "2026-10-01T00:00:00Z"), brought("b2", "2026-10-02T00:00:00Z")]);
  assert.equal(groups.length, 1);
  assert.equal(groups[0].brought, true);
  assert.equal(isBrought(groups[0].latest), true);
  assert.equal(broughtFrom(groups[0].latest), "drive Docs: partner.json");
  assert.deepEqual(groups[0].runs, []);
});

test("what a standalone model trains on is read from its pipeline's model step", () => {
  const graph = {
    nodes: [
      { id: "load", kind: "connect", attributes: { source: "datasets", table: "accounts" } },
      { id: "fit", kind: "model", attributes: { standalone_training: { dataset_path: "datasets.accounts", target: "churned", methodology: "M-1", holdout_fraction: 0.2 } } },
    ],
  };
  assert.deepEqual(trainingOf(graph), { dataset: "datasets.accounts", target: "churned", methodology: "M-1", entity: "", holdout: 0.2 });
  assert.equal(trainingOf({ nodes: [{ id: "fit", kind: "model", attributes: {} }] }), null);
  assert.equal(trainingOf(null), null);
});

test("a methodology needs a target when its label input is required", () => {
  assert.equal(needsTarget({ inputs: [{ role: "entity", required: true }, { role: "label", required: true }] }), true);
  assert.equal(needsTarget({ inputs: [{ role: "entity", required: true }, { role: "feature", required: true }] }), false);
  assert.equal(needsTarget({ inputs: [{ role: "label", required: false }] }), false);
  assert.equal(needsTarget(null), false);
});

test("a new model's name and held-out share are checked before they are sent", () => {
  const groups = groupModels([standalone("s1", "2026-10-07T10:00:00Z"), ...records]);
  assert.equal(nameProblem("  ", groups), "Name the model.");
  assert.match(nameProblem("churn", groups)!, /exists/);
  assert.equal(nameProblem("upsell", groups), null, "a project's model of that name is not a standalone model");
  assert.deepEqual(holdoutOf(""), { value: null });
  assert.deepEqual(holdoutOf("0.25"), { value: 0.25 });
  assert.deepEqual(holdoutOf("20%"), { value: 0.2 });
  assert.ok("error" in holdoutOf("1.5"));
  assert.ok("error" in holdoutOf("abc"));
});

test("a workspace methodology's model is served by its own code (lake#690)", () => {
  assert.equal(methodologyOf({ id: "m", kind: "python:W-0123abcd" }), "W-0123abcd");
  assert.equal(methodologyOf({ id: "m", kind: "python:M-1" }), "");
  assert.equal(methodologyOf({ id: "m", kind: "gbdt" }), "");
  assert.equal(methodologyOf(undefined), "");

  assert.deepEqual(serverState(null), { label: "not served", tone: "neutral", settled: true, ready: false });
  assert.equal(serverState({ phase: "Ready" }).ready, true);
  assert.equal(serverState({ phase: "Staging" }).settled, false, "refreshed while on its way");
  assert.equal(serverState({ phase: "Queued" }).tone, "warning");
  assert.equal(serverState({ phase: "Failed" }).tone, "danger");
  assert.equal(serverState({ phase: "Stopped" }).label, "not served");
  assert.equal(serverState({}).label, "pending");

  const answer = { runner: "methodology", predictions: [{ row: 0, entity: "c-1", prediction: 0.25 }, { row: 1, prediction: "yes" }] };
  assert.equal(predictionRows(answer).length, 2);
  assert.deepEqual(predictionRows({ ranked: [] }), [], "an engine model answers ranked rows instead");
});
