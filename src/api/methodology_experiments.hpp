#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"

// Experiments over a workspace methodology's parameters (lake#689, docs/api/methodologies.md
// #experiments). The engine samples the declared parameters and admits each trial as a Run that
// waits for Lake's runner, listed in the experiment's `runner` work items. Lake runs each as a
// training run of the methodology (the Orama operator's sandbox, metered as every run is) and
// reports the result back with record_methodology_run (`study`, `trial`), which admits the next
// trials; a run that ends without a result ends its trial with the reason (`stop_trial`). Which
// Lake run serves which trial is a sealed `lake_methodology_trials` record in the workspace Zetta,
// written before the run is queued, so a trial is dispatched once.
namespace lake::api {

// An experiment as the engine answered it, made current: for a workspace methodology's running
// experiment, every waiting trial without a Lake run is dispatched, and every trial whose Lake run
// ended without a result is ended with its reason (the engine then admits the next). Answers the
// experiment with `runner` (each waiting trial with its `methodology_run`) and `methodology_runs`
// (every trial's Lake run: trial, number, run_id, phase, message). An experiment of Orama's
// methodologies comes back unchanged.
liquid::runtime::task<nlohmann::json> current_experiment(ApiContext& context,
                                                         const std::string& customer_id,
                                                         const std::string& workspace_id,
                                                         nlohmann::json experiment);

// Withdraws the Lake runs of an experiment's trials that are still running: every trial's, or the
// one trial's (its engine Run id) when `trial` is given.
liquid::runtime::task<void> stop_experiment_runs(ApiContext& context,
                                                 const std::string& customer_id,
                                                 const std::string& workspace_id,
                                                 const std::string& experiment_id,
                                                 const std::string& trial = {});

} // namespace lake::api
