/**
 * The Experiments charts' transformations (moved from the server's experiments.js test,
 * tests/experiment_charts.cjs, lake#561): smoothing, missing steps, time axes, log scale,
 * differential privacy, parameter differences and the live-read cursor.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  artifactChart,
  artifactSource,
  artifactTitle,
  canCompare,
  chosenRuns,
  ema,
  heldOutScores,
  metricData,
  nextCursor,
  parameterDiff,
  type RunDetail,
} from "../src/pages/experiments/transforms.ts";

const points = [
  { step: 1, value: 4, time: "2026-09-28T00:00:00Z" },
  { step: 3, value: 2, time: "2026-09-28T00:00:02Z" },
  { step: 4, value: 0, time: "2026-09-28T00:00:03Z" },
];
const runs = [
  { run: "a", params: { rate: 0.1, model: "gbdt" }, series: [{ name: "train.loss", points }] },
  {
    run: "b",
    params: { rate: 0.2, model: "gbdt", extra: true },
    series: [{ name: "train.loss", points: [{ step: 2, value: 8, time: "2026-09-28T00:00:01Z" }] }],
  },
] as unknown as RunDetail[];

test("smoothing is an exponential moving average and leaves the points alone", () => {
  assert.deepEqual(ema(points, 0).map((point) => point.value), [4, 2, 0]);
  assert.deepEqual(ema(points, 0.5).map((point) => point.value), [4, 3, 1.5]);
  assert.deepEqual(points.map((point) => point.value), [4, 2, 0]);
});

test("series line up on the axis, with gaps for missing steps", () => {
  assert.deepEqual(metricData(runs, "train.loss", "step", 0, false), [
    [1, 2, 3, 4],
    [4, null, 2, 0],
    [null, 8, null, null],
  ]);
  assert.deepEqual(metricData(runs, "train.loss", "step", 0, true)[1], [4, null, 2, null]);
  assert.deepEqual(metricData(runs, "train.loss", "relative", 0, false), [
    [0, 2, 3],
    [4, 2, 0],
    [8, null, null],
  ]);
  assert.equal(metricData(runs, "train.loss", "wall", 0, false)[0][0], Date.parse(points[0].time) / 1000);
});

test("a run under differential privacy draws nothing", () => {
  const suppressed = [{ ...runs[0], metrics_suppressed: "differential_privacy" }] as unknown as RunDetail[];
  assert.deepEqual(metricData(suppressed, "train.loss", "step", 0, false), [[], []]);
});

test("parameters differ by name, and absent ones show a dash", () => {
  assert.deepEqual(parameterDiff(runs), [
    { name: "extra", values: ["—", "true"], differs: true },
    { name: "model", values: ['"gbdt"', '"gbdt"'], differs: false },
    { name: "rate", values: ["0.1", "0.2"], differs: true },
  ]);
});

test("the live read resumes after the last event it saw", () => {
  assert.equal(nextCursor({ events: [{ sequence: 10 }, { sequence: 11 }], next_sequence: 100 }, 10), 12);
  assert.equal(nextCursor({ events: [], next_sequence: 100 }, 12), 100);
  assert.equal(nextCursor({ events: [{ sequence: 1 }], next_sequence: 2 }, 12), 12);
});

test("held-out scores line up per run, with the best marked (zetta#1239)", () => {
  const details = [
    { run: "r1", summary: { held_out_auc: { final: 0.874 }, held_out_log_loss: { final: 0.41 }, held_out_rows: { final: 300 }, "train.log_loss": { final: 0.2 } } },
    { run: "r2", summary: { held_out_auc: { final: 0.89 }, held_out_log_loss: { final: 0.383 }, held_out_rows: { final: 292 } } },
    { run: "r3", summary: { "train.log_loss": { final: 0.1 } } },
    { run: "dp", metrics_suppressed: "differential_privacy", summary: { held_out_auc: { final: 0.99 } } },
  ] as unknown as RunDetail[];
  const scores = heldOutScores(details);
  assert.deepEqual(scores.metrics, ["held_out_auc", "held_out_log_loss", "held_out_rows"]);
  assert.deepEqual(
    scores.rows.map((row) => [row.run, ...row.values]),
    [
      ["r1", 0.874, 0.41, 300],
      ["r2", 0.89, 0.383, 292],
      ["r3", null, null, null],
    ],
  );
  assert.deepEqual(scores.best, ["r2", "r2", null], "AUC higher is better, log loss lower; rows are not a score");
  assert.deepEqual(heldOutScores([details[2]]), { metrics: [], rows: [{ run: "r3", values: [] }], best: [] });
  assert.deepEqual(heldOutScores([details[0]]).best, [null, null, null], "one run has nothing to beat");
});

test("Compare Runs compares only two to eight chosen runs (lake#576)", () => {
  assert.deepEqual(chosenRuns(""), []);
  assert.deepEqual(chosenRuns(null), []);
  assert.deepEqual(chosenRuns("a"), ["a"]);
  assert.deepEqual(chosenRuns(" a , b,,a "), ["a", "b"]);
  assert.equal(canCompare(chosenRuns("")), false);
  assert.equal(canCompare(chosenRuns("a")), false);
  assert.equal(canCompare(chosenRuns("a,a")), false, "the same run twice is one run");
  assert.equal(canCompare(chosenRuns("a,b")), true);
  assert.equal(canCompare(chosenRuns("a,b,c,d,e,f,g,h,i")), false);
});

// zetta#1247 (lake#615): a GBDT classifier logs its held-out confusion matrix, ROC curve and
// calibration; the run page names each chart and its axes, and draws the line to read it against.
test("the held-out charts are named and drawn on their own axes", () => {
  assert.equal(artifactTitle("confusion_matrix"), "Confusion Matrix");
  assert.equal(artifactTitle("roc_curve"), "ROC Curve");
  assert.equal(artifactTitle("calibration"), "Calibration");
  assert.equal(artifactTitle("lift_chart"), "lift chart");
  assert.equal(artifactSource("held_out"), "held-out rows");
  assert.equal(artifactSource("validation"), "validation");

  const roc = artifactChart("roc_curve", { x: [0, 0.25, 1], y: [0, 0.75, 1], thresholds: [1, 0.6, 0.1] });
  assert.deepEqual(roc.data, [[0, 0.25, 1], [0, 0.75, 1], [0, 0.25, 1]], "the curve and the chance diagonal");
  assert.deepEqual(roc.labels, ["True positive rate", "Chance"]);
  assert.equal(roc.settings.xLabel, "False positive rate");

  const calibration = artifactChart("calibration", { predicted: [0.12, 0.81], observed: [0.1, 0.9], counts: [40, 60] });
  assert.deepEqual(calibration.data, [[0.12, 0.81], [0.1, 0.9], [0.12, 0.81]]);
  assert.equal(calibration.settings.xLabel, "Mean predicted probability");
  assert.deepEqual(calibration.labels, ["Observed rate", "Perfectly calibrated"]);

  const importance = artifactChart("feature_importance", { features: ["tenure", "usage"], values: [0.4, 0.6] });
  assert.deepEqual(importance.settings.xValues, ["tenure", "usage"]);
  assert.equal(artifactChart("pr_curve", { x: [0, 1], y: [1, 0.5] }).settings.xLabel, "Recall");
  assert.deepEqual(artifactChart("roc_curve", { x: [0, "bad", 1], y: [0, 1] }).data[0], [0, null, 1], "a value that is not a number is a gap");
});
