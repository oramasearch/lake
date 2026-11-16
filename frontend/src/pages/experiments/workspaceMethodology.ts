// SPDX-License-Identifier: MIT

/** Experiments over a workspace methodology's parameters (lake#689): what the Start an Experiment
 * drawer offers for one, and what a trial's Lake run says. The engine publishes, for every
 * methodology an experiment can search, `studies: true` and its `study_search_space`; a workspace
 * methodology's (source `workspace`) is its declared parameters, and its trials run in Lake's
 * runner, one training run each. */
import type { Row } from "@/lib/api";

export type StudyParameter = { name: string; kind: string; minimum?: number; maximum?: number; log?: boolean; choices?: unknown[]; default?: unknown };

/** The methodologies an experiment can search: Orama's and the workspace's own, each `studies: true`. */
export function studyMethodologies(listed: unknown): Row[] {
  const all = Array.isArray(listed) ? (listed as Row[]) : [];
  return all.filter((entry) => entry && entry.studies === true && typeof entry.id === "string");
}

/** A workspace methodology: Python run by Lake's runner, not Orama's catalogue. */
export function isWorkspaceMethodology(entry: Row | undefined): boolean {
  return !!entry && (entry.source === "workspace" || /^W-[0-9a-f]{8}$/.test(String(entry.id ?? "")));
}

/** The parameters its experiment searches, as declared. */
export function searchedParameters(entry: Row | undefined): StudyParameter[] {
  const space = (entry?.study_search_space ?? {}) as Row;
  return Array.isArray(space.parameters) ? (space.parameters as StudyParameter[]) : [];
}

/** One parameter's range in words: `0.001 – 1 (log)`, `1 – 16`, `false, true`. */
export function parameterRange(parameter: StudyParameter): string {
  if (parameter.kind === "categorical") return (parameter.choices ?? []).map((choice) => String(choice)).join(", ");
  const range = `${parameter.minimum ?? "?"} – ${parameter.maximum ?? "?"}`;
  return parameter.log ? `${range} (log)` : range;
}

/** The metrics it can be compared by (`held_out_<m>`), in the order it reports them. */
export function objectiveMetrics(entry: Row | undefined): string[] {
  const space = (entry?.study_search_space ?? {}) as Row;
  return Array.isArray(space.objective_metrics) ? (space.objective_metrics as string[]) : [];
}

/** A metric that is better smaller: a loss or an error. */
export function minimized(metric: string): boolean {
  return /loss|error|rmse|mse|mae|mape|brier|perplexity|inertia/.test(metric);
}

/** Whether it learns a target column. */
export function needsTargetColumn(entry: Row | undefined): boolean {
  const target = ((entry?.interface ?? {}) as Row).target as Row | undefined;
  return target ? target.required === true : true;
}

/** What a trial's Lake run says, by the trial's run id: phase and message. */
export function trialRuns(experiment: Row | null | undefined): Map<string, Row> {
  const runs = new Map<string, Row>();
  const listed = experiment?.methodology_runs;
  if (Array.isArray(listed)) {
    for (const run of listed as Row[]) {
      if (typeof run.trial === "string") runs.set(run.trial, run);
    }
  }
  return runs;
}
