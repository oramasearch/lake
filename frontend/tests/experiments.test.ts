/**
 * The Experiments charts' transformations (moved from the server's experiments.js test,
 * tests/experiment_charts.cjs, lake#561): smoothing, missing steps, time axes, log scale,
 * differential privacy, parameter differences and the live-read cursor.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { ema, metricData, nextCursor, parameterDiff, type RunDetail } from "../src/pages/experiments/transforms.ts";

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
