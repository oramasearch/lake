// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"

// Serving a workspace methodology's model from Ship → Models (lake#690,
// docs/api/methodologies.md#serving-its-model): the two calls workspace_capabilities.cpp and
// deletion_capabilities.cpp make into methodology_capabilities.cpp.
namespace lake::api {

// Publishing (`serve` true) a model a workspace methodology trained starts serving it; unpublishing
// or deleting it (`serve` false) stops whatever serves it. Answers {deployment} for such a model,
// null for any other, or the refusal.
liquid::runtime::task<ApiResult> serve_methodology_model(ApiContext& context,
                                                         const std::string& customer_id,
                                                         const std::string& workspace_id,
                                                         const std::string& model_id,
                                                         bool serve);

// `predict` of such a model: the rows go to its serve pod and its predictions come back, one per
// row. `model` is the engine's get_model answer. nullopt when the model is not a workspace
// methodology's.
liquid::runtime::task<std::optional<ApiResult>>
predict_methodology_model(ApiContext& context,
                          const std::string& customer_id,
                          const std::string& workspace_id,
                          const nlohmann::json& model,
                          const nlohmann::json& input);

} // namespace lake::api
