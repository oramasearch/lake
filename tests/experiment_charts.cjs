const assert = require('node:assert/strict');
const {ema, metricData, parameterDiff, nextCursor} = require('../assets/experiments/experiments.js');

const points = [
  {step: 1, value: 4, time: '2026-09-28T00:00:00Z'},
  {step: 3, value: 2, time: '2026-09-28T00:00:02Z'},
  {step: 4, value: 0, time: '2026-09-28T00:00:03Z'}
];
const runs = [
  {run: 'a', params: {rate: 0.1, model: 'gbdt'}, series: [{name: 'train.loss', points}]},
  {run: 'b', params: {rate: 0.2, model: 'gbdt', extra: true}, series: [{name: 'train.loss',
    points: [{step: 2, value: 8, time: '2026-09-28T00:00:01Z'}]}]}
];
assert.deepEqual(ema(points, 0).map(point => point.value), [4, 2, 0]);
assert.deepEqual(ema(points, 0.5).map(point => point.value), [4, 3, 1.5]);
assert.deepEqual(metricData(runs, 'train.loss', 'step', 0, false),
  [[1, 2, 3, 4], [4, null, 2, 0], [null, 8, null, null]]);
assert.deepEqual(metricData(runs, 'train.loss', 'step', 0, true)[1], [4, null, 2, null]);
assert.deepEqual(metricData(runs, 'train.loss', 'relative', 0, false),
  [[0, 2, 3], [4, 2, 0], [8, null, null]]);
assert.equal(metricData(runs, 'train.loss', 'wall', 0, false)[0][0],
  Date.parse(points[0].time) / 1000);
assert.deepEqual(metricData([{...runs[0], metrics_suppressed: 'differential_privacy'}],
  'train.loss', 'step', 0, false), [[], []]);
assert.deepEqual(parameterDiff(runs), [
  {name: 'extra', values: ['—', 'true'], differs: true},
  {name: 'model', values: ['"gbdt"', '"gbdt"'], differs: false},
  {name: 'rate', values: ['0.1', '0.2'], differs: true}
]);
assert.equal(nextCursor({events: [{sequence: 10}, {sequence: 11}], next_sequence: 100}, 10), 12);
assert.equal(nextCursor({events: [], next_sequence: 100}, 12), 100);
assert.equal(nextCursor({events: [{sequence: 1}], next_sequence: 2}, 12), 12);
assert.deepEqual(points.map(point => point.value), [4, 2, 0]);
console.log('experiment-charts: EMA, missing steps, time axes, log scale, privacy, params and cursors passed');
