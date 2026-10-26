# Feature Planning: `plan_features` From a Dataset Alone

**Status: implemented 2026-10-25 in the source tree (lake#1153); not deployed.**

`plan_features` reads a dataset in a workspace and answers how a methodology would turn its columns
into features: what goes in, what is left out and why, or the preparation the data needs first
(`status: "transform_required"`). The plan itself is the workspace engine's (zetta's
`docs/data/featurization.md`); this page is what Lake adds around it. It applies to the top-level
`plan_features` capability and to `workspace_query {capability: "plan_features"}` alike, so Auto-Suggest Agent
(Design → Opportunities), Ask Orama and a person over MCP get the same plan.

## Why

A clean churn table (one row per account, an `account` key, a `churned` label, an `as_of` date and a
90-day window) got no Opportunity from *Scan my workspace* (lake#1153). Two things in the plan stood
in the way:

- The engine requires a `methodology`, but Lake's published schema did not say so: `plan_features
  {dataset}` answered `invalid_input: no subschema has succeeded, but one of them is required`.
- The engine refuses to plan any table with a date column unless the caller declares the cutoff its
  features are computed at (`as_of`), even when every row already carries it. Auto-Suggest Agent never
  passed one, so every such table came back `add_snapshot_date`, and the scan declined it as
  *transform first*, asking for an "approved snapshot/as_of setup" that no Lake page or capability
  offers.

## What Lake fills in

| Left out | What Lake does | Said in the plan as |
|---|---|---|
| `methodology` | Analyzes the dataset (`analyze_dataset`): M-1 (classification) when a column is a label (`label`, or a column the analysis names a label), M-3 (segmentation) when none is | `inferred.methodology: {id, reason}` |
| `as_of` | Takes the column that holds each row's snapshot date (`time_column` when given, else the first column named `as_of`, `asof`, `as_of_date`, `snapshot`, `snapshot_date`, `snapshot_at`, `observed_at`, `observed_on`, `observation_date`, `cutoff`, `cutoff_date`, `prediction_date`, `scored_at`, `extracted_at`, ...) and reads its latest value with one read-only `SELECT <column> ... ORDER BY <column> DESC LIMIT 16`. The latest ISO date (`2026-09-27`, with or without a time) or epoch number is passed as `as_of`, and the column as `time_column` | `inferred.as_of: {column, value, reason}` |

A methodology or `as_of` the caller names is used as named, and Lake reads nothing to find one.
A date column named otherwise (`signup_date`, `last_login`) is a feature, never the cutoff, so Lake
does not take one for it: the engine still asks for `as_of`, and the plan says why Lake did not fill
it (`transform_required.why_no_as_of`). Nothing in the data changes in any case.

`dataset_as_of` (`src/api/feature_planning.hpp`) is the one way Lake reads a dataset's cutoff. Any
capability that needs the snapshot a dataset was observed at (training a model on a table with
dates, lake#1181) calls it rather than carrying its own rule.

## What performs a transform

When the engine still answers `transform_required`, Lake adds `performed_by`: the Lake capabilities
that do the preparation, each `{capability, does, input?}`.

| Engine transform | `performed_by` |
|---|---|
| `add_snapshot_date` | `plan_features` with `as_of` (the date the rows were observed at; and `time_column` naming the row's snapshot column, if there is one), or `upload_dataset` with an `as_of` column |
| `aggregate_events_to_entity` | `workspace_sql` with the engine's proposed `SELECT` (one row per entity), then `upload_dataset` of those rows |
| `join_label_table` | `upload_dataset` of the joined table (the workspace SQL has no JOIN) |
| `add_time_column`, `add_entity_key`, `capture_label`, `add_ranking_group`, `add_treatment`, `pivot_long_to_wide`, any other | `upload_dataset` of the table with the field added |

Auto-Suggest Agent names these in a *transform first* answer, so a person is never told to do something
Lake offers no way to do.

## Example

```json
{"dataset": "datasets.ux_review_churn"}
```

answers, on the issue's table:

```json
{
  "status": "ready",
  "dataset": "datasets.ux_review_churn",
  "methodology": "M-1",
  "entries": ["..."],
  "excluded": ["..."],
  "transform_required": null,
  "inferred": {
    "methodology": {"id": "M-1", "reason": "no methodology was named and churned is a label, so it is planned for classification (M-1)"},
    "as_of": {"column": "as_of", "value": "2026-09-27", "reason": "no as_of was named; the latest as_of in the dataset is the cutoff its features are computed at"}
  }
}
```

## Verify

- `Workspaces.PlanFeaturesTakesADatasetAloneAndReadsItsSnapshotCutoff` (`tests/test_workspaces.cpp`,
  a test double of the engine): each shape of lake#1153, the statement Lake sends, a named
  methodology or cutoff kept, a date that is not a snapshot, `performed_by` for a snapshot and an
  event table, and the refusal when the dataset cannot be analyzed.
- `FeaturePlanZetta.EachChurnShapeFromTheIssuePlansFromTheDatasetAlone` on a real Zetta
  (`LAKE_TEST_ZETTA_URL`, `LAKE_TEST_ZETTA_TOKEN`): the issue's three shapes plan `ready` for M-1, M-2
  and M-3 (S-1 level 1), `plan_features {dataset}` alone answers M-1 with the `as_of` cutoff, and a
  `signup_date` table names `plan_features` and `upload_dataset`, then plans `ready` with `as_of`.

## How to change it

- A new snapshot column name: `snapshot_names` in `src/api/feature_planning.cpp`.
- A new engine transform: a branch in `performed_by` naming the Lake capability that performs it.
- The default methodology: `prepare_feature_plan`. Keep it a choice the plan reports in `inferred`.
