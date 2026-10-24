#pragma once

// What the two stream workers share (lake#652, lake#677): acting as the person who set a stream,
// and starting one job of an agent for each new message on a workspace stream. The MCP connection
// worker (mcp_connection_streams.cpp) and the agent stream-trigger worker
// (agent_stream_triggers.cpp) both start their runs here, so a message starts a run the same way
// whichever of them reads it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/agent_bus.hpp"

namespace lake::api {

// The person a stream acts as: a context authorized as that user, never as the operator, with
// `method` as its authentication method in the audit. Missing/disabled users are refused;
// storage errors remain distinct so workers do not discard configuration during an outage.
control::Result<ApiContext>
stream_person(const ApiContext& base, const std::string& user_id, std::string method);

struct StreamRuns {
    std::uint64_t cursor = 0;          // the last sequence read (each one starts at most one run)
    std::vector<std::string> launched; // the launch ids of the runs started
    std::vector<std::string> problems; // why a run did not start, in Lake's words
    std::string unreadable;            // why the stream could not be read; empty when it was
    bool agent_gone = false;           // launch_agent answered not_found for the agent
};

// Reads at most `max_events` messages after `cursor` on `topic` of the workspace and starts one job
// of `agent_id` for each (`launch_agent`, mode `job`, as `person`), with `input_of(message)` as the
// job's input. The cursor moves past every message read, whether or not its run started, so a
// message never starts two runs.
StreamRuns
start_stream_runs(const ApiContext& context,
                  const ApiContext& person,
                  control::AgentBus& bus,
                  const std::string& customer_id,
                  const std::string& workspace_id,
                  const std::string& agent_id,
                  const std::string& topic,
                  std::uint64_t cursor,
                  std::size_t max_events,
                  const std::function<nlohmann::json(const control::BusEvent&)>& input_of);

} // namespace lake::api
