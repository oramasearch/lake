/** The workspace's agent model picker (lake#1230, docs/ui/workspace-selection.md#settings): the default,
 * the labels, the prices per million tokens and what is sent. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_AGENT_MODEL,
  agentModelInput,
  agentModelLabel,
  agentModelPrice,
  currentAgentModel,
  defaultAgentModel,
  longPromptPrice,
  type WorkspaceSettings,
} from "../src/lib/agentModels.ts";

const sol = {
  model: "gpt-6.1-sol",
  default: true,
  input_micros_per_million: 2_600_000,
  output_micros_per_million: 13_000_000,
  long_input_micros_per_million: 5_200_000,
  long_output_micros_per_million: 19_500_000,
};
const luna = { model: "gpt-6-luna", default: false, input_micros_per_million: 130_000, output_micros_per_million: 650_000, long_input_micros_per_million: 260_000, long_output_micros_per_million: 975_000 };
const gpt4o = { model: "gpt-4o", input_micros_per_million: 3_250_000, output_micros_per_million: 13_000_000, long_input_micros_per_million: null, long_output_micros_per_million: null };

test("unset means gpt-5.5 (lake#1245)", () => {
  assert.equal(DEFAULT_AGENT_MODEL, "gpt-5.5");
  assert.equal(currentAgentModel(null), "gpt-5.5");
  assert.equal(currentAgentModel({ agent_model: "gpt-5.5", agent_model_chosen: null }), "gpt-5.5");
  assert.equal(currentAgentModel({ agent_model: "gpt-6-luna", agent_model_chosen: "gpt-6-luna" }), "gpt-6-luna");
  assert.equal(defaultAgentModel({ default_agent_model: "gpt-6.1-terra" }), "gpt-6.1-terra");
});

test("the default is labelled, the others by name", () => {
  assert.equal(agentModelLabel(sol), "gpt-6.1-sol (default)");
  assert.equal(agentModelLabel(luna), "gpt-6-luna");
});

test("each model shows its price per million tokens from the list in force", () => {
  assert.equal(agentModelPrice(sol, "USD"), "$2.60 in, $13.00 out per million tokens");
  assert.equal(agentModelPrice(luna, "USD"), "$0.13 in, $0.65 out per million tokens");
  assert.equal(longPromptPrice(sol, "USD"), "Prompts over 272,000 tokens: $5.20 in, $19.50 out per million tokens");
  assert.equal(longPromptPrice(gpt4o, "USD"), null, "a model with one rate has no long-prompt line");
});

test("without a published price it says so, never $0", () => {
  assert.equal(agentModelPrice({ model: "gpt-5", input_micros_per_million: null, output_micros_per_million: null }, "USD"), "No price published on this Lake");
  assert.equal(agentModelPrice(sol, null), "No price published on this Lake");
  assert.equal(longPromptPrice(sol, null), null);
});

test("choosing the default goes back to unset; any other model is sent by name", () => {
  const settings: WorkspaceSettings = { default_agent_model: "gpt-6.1-sol" };
  assert.deepEqual(agentModelInput("gpt-6.1-sol", settings), { agent_model: null });
  assert.deepEqual(agentModelInput("gpt-6-luna", settings), { agent_model: "gpt-6-luna" });
  assert.deepEqual(agentModelInput("gpt-5.5", null), { agent_model: null });
});
