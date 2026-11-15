// SPDX-License-Identifier: MIT

/**
 * Experiments over a workspace methodology's parameters (lake#689): which methodologies the Start an
 * Experiment drawer offers, how a workspace methodology's declared parameters read, its objective
 * metrics, and a trial's Lake run.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  isWorkspaceMethodology,
  minimized,
  needsTargetColumn,
  objectiveMetrics,
  parameterRange,
  searchedParameters,
  studyMethodologies,
  trialRuns,
} from "../src/pages/experiments/workspaceMethodology.ts";

const own = {
  id: "W-0a1b2c3d",
  name: "Late payers",
  source: "workspace",
  studies: true,
  interface: { target: { required: true, kind: "binary" } },
  study_search_space: {
    parameters: [
      { name: "learning_rate", kind: "float", minimum: 0.001, maximum: 1, log: true },
      { name: "epochs", kind: "int", minimum: 1, maximum: 5000 },
      { name: "balanced", kind: "categorical", choices: [false, true] },
    ],
    objective_metrics: ["held_out_auc", "held_out_log_loss"],
  },
};

test("only methodologies an experiment can search are offered, Orama's and the workspace's", () => {
  const offered = studyMethodologies([
    { id: "M-1", source: "orama", studies: true },
    { id: "M-3", source: "orama", studies: false },
    own,
    { id: "W-ffffffff", source: "workspace", studies: false, study_reason: "Validate W-ffffffff first" },
  ]);
  assert.deepEqual(
    offered.map((entry) => entry.id),
    ["M-1", "W-0a1b2c3d"],
  );
  assert.equal(isWorkspaceMethodology(own), true);
  assert.equal(isWorkspaceMethodology({ id: "M-1", source: "orama" }), false);
  assert.deepEqual(studyMethodologies(undefined), []);
});

test("a workspace methodology's declared parameters read as their ranges", () => {
  const parameters = searchedParameters(own);
  assert.deepEqual(parameters.map(parameterRange), ["0.001 – 1 (log)", "1 – 5000", "false, true"]);
  assert.deepEqual(searchedParameters(undefined), []);
});

test("its trials are compared by its held-out metrics, a loss minimized", () => {
  assert.deepEqual(objectiveMetrics(own), ["held_out_auc", "held_out_log_loss"]);
  assert.equal(minimized("held_out_log_loss"), true);
  assert.equal(minimized("held_out_rmse"), true);
  assert.equal(minimized("held_out_auc"), false);
  assert.equal(needsTargetColumn(own), true);
  assert.equal(needsTargetColumn({ interface: { target: { required: false, kind: "none" } } }), false);
});

test("a trial's Lake run is found by the trial's run id", () => {
  const runs = trialRuns({
    methodology_runs: [
      { trial: "run-0", number: 0, run_id: "lake-run-0", phase: "Running", message: "Running your code in its sandbox." },
      { trial: "run-1", number: 1, run_id: null, phase: "Pending", message: "Waiting for Lake to queue this trial's run." },
    ],
  });
  assert.equal(runs.get("run-0")?.phase, "Running");
  assert.equal(runs.get("run-1")?.run_id, null);
  assert.equal(trialRuns(null).size, 0);
});
