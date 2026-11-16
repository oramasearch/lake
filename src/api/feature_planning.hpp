// SPDX-License-Identifier: MIT

#pragma once

// Feature planning for a dataset in a workspace (lake#1153, docs/api/feature-planning.md).
//
// The workspace engine's plan_features needs a methodology, and refuses to plan a table with a
// date column unless the caller declares the cutoff (`as_of`) its features are computed at. Lake
// fills both from the data when the caller leaves them out, says what it filled, and names the
// Lake capability that performs any preparation the engine still asks for. dataset_as_of is the
// one way Lake reads a dataset's snapshot cutoff, for any capability that needs it.

#include <expected>
#include <string>
#include <string_view>

#include "lake/api/capabilities.hpp"

namespace lake::api {

// Whether a column's name says it holds the date each row was observed at (`as_of`,
// `snapshot_date`, `observed_at`, ...), compared without case.
bool is_snapshot_column(std::string_view name);

// A dataset's snapshot cutoff: the column holding each row's snapshot date and its latest value.
struct DatasetAsOf {
    std::string column;
    std::string value;
};

// Reads the cutoff of the dataset `reference` names (`dataset`, with `database` for a bare name,
// or `dataset_path`): `time_column` when given, else the first column named like a snapshot date,
// and the latest ISO date or epoch value in it. Answers why not when there is none.
liquid::runtime::task<std::expected<DatasetAsOf, std::string>>
dataset_as_of(ApiContext& context,
              std::string customer_id,
              const nlohmann::json& reference,
              std::string time_column);

// Before plan_features reaches the engine: a missing `methodology` is chosen from the dataset's
// analysis (M-1 when it has a label, else M-3), and a missing `as_of` is read with dataset_as_of.
// Answers what was filled, as the `inferred` object of the plan, or a refusal.
liquid::runtime::task<std::expected<nlohmann::json, ApiResult>>
prepare_feature_plan(ApiContext& context, std::string customer_id, nlohmann::json& arguments);

// After plan_features answers: `inferred` is added when anything was filled, and a
// `transform_required` names the Lake capabilities that perform it (`performed_by`).
void describe_feature_plan(nlohmann::json& plan, const nlohmann::json& inferred);

} // namespace lake::api
