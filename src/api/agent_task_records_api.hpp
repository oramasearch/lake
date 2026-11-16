// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/capabilities.hpp"
#include "lake/control/agent_task_records.hpp"
#include "lake/control/store.hpp"

namespace lake::api {

// An agent task's two halves (lake#374, lake#375, docs/api/agent-bus.md). The control store holds
// its dispatch row; what a person or an agent wrote for it (the request, the agent's message and
// its answer) is one sealed record in the workspace Zetta of the task's own workspace, reached
// through call_workspace, which checks the workspace's space and readiness on every call. Nothing
// falls back to PostgreSQL or to another workspace. Lake assembles the task callers see from both.

template <class T> using AgentTaskTask = liquid::runtime::task<control::ZettaControlResult<T>>;

// Fills in `task` from its record: `body` (the request), `result`, and `message` unless lake's
// own notice is set, which outranks it. Answers whether the record exists: a task without one yet
// (created before the move and not copied) keeps an empty body, no answer and lake's notice.
// Fails only when the workspace could not be read.
AgentTaskTask<bool> load_agent_task_content(const ApiContext& context, control::AgentTask& task);

// The control store's task with its content: `not_found` when there is no such task.
AgentTaskTask<control::AgentTask> agent_task_with_content(const ApiContext& context,
                                                          std::string task_id);

// Writes a new task's request (`task.body`) into its workspace before its dispatch row exists, so
// an agent that claims it always finds it. `task.id` is the id the row will take.
AgentTaskTask<void> write_agent_task_request(const ApiContext& context,
                                             const control::AgentTask& task);

// Records what an event the control store accepted said: the message, and for Succeeded or
// Failed the answer. A conditional write on the revision read; a record that already holds a
// finished answer keeps it.
AgentTaskTask<void> write_agent_task_event(const ApiContext& context,
                                           const control::AgentTask& task, std::string phase,
                                           std::string message, nlohmann::json result);

} // namespace lake::api
