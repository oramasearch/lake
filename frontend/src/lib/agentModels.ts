// SPDX-License-Identifier: MIT

/** The model a workspace's agents call (lake#1230, docs/ui/workspace-selection.md#settings): one of the
 * chat models this Lake serves, read with `get_workspace_settings` and chosen with
 * `set_workspace_settings`; unset means the default, gpt-6.1-sol. Each model shows its price per
 * million tokens from the price list in force. Pure, so `tests/agent-models.test.ts` runs it under
 * Node. */
import { money } from "../pages/billing/billing.ts";

export type AgentModelOption = {
  model: string;
  default?: boolean;
  input_micros_per_million?: number | null;
  output_micros_per_million?: number | null;
  long_input_micros_per_million?: number | null;
  long_output_micros_per_million?: number | null;
};

export type WorkspaceSettings = {
  workspace_id?: string;
  agent_model?: string;
  agent_model_chosen?: string | null;
  default_agent_model?: string;
  currency?: string | null;
  updated_at?: string | null;
  agent_models?: AgentModelOption[];
};

/** The default when the answer does not say (an older Lake). */
export const DEFAULT_AGENT_MODEL = "gpt-6.1-sol";

/** The prompt length past which a request is billed at the long-context prices. */
export const LONG_PROMPT_TOKENS = 272_000;

export function defaultAgentModel(settings: WorkspaceSettings | null | undefined): string {
  return settings?.default_agent_model || DEFAULT_AGENT_MODEL;
}

/** The model the agents call now: the one chosen, else the default. */
export function currentAgentModel(settings: WorkspaceSettings | null | undefined): string {
  return settings?.agent_model_chosen || settings?.agent_model || defaultAgentModel(settings);
}

/** "gpt-6.1-sol (default)" or "gpt-6-luna". */
export function agentModelLabel(option: AgentModelOption, defaultModel = DEFAULT_AGENT_MODEL): string {
  return option.model === defaultModel || option.default ? `${option.model} (default)` : option.model;
}

const priced = (value: number | null | undefined): value is number => typeof value === "number" && Number.isFinite(value);

/** "$2.00 in, $10.00 out per million tokens", or why there is no price. */
export function agentModelPrice(option: AgentModelOption, currency: string | null | undefined): string {
  if (!currency || !priced(option.input_micros_per_million) || !priced(option.output_micros_per_million)) {
    return "No price published on this Lake";
  }
  return `${money(option.input_micros_per_million, currency)} in, ${money(option.output_micros_per_million, currency)} out per million tokens`;
}

/** What a prompt over 272,000 tokens costs, or null when the model has no long-context price. */
export function longPromptPrice(option: AgentModelOption, currency: string | null | undefined): string | null {
  if (!currency || !priced(option.long_input_micros_per_million) || !priced(option.long_output_micros_per_million)) return null;
  return `Prompts over ${LONG_PROMPT_TOKENS.toLocaleString("en-US")} tokens: ${money(option.long_input_micros_per_million, currency)} in, ${money(option.long_output_micros_per_million, currency)} out per million tokens`;
}

/** What `set_workspace_settings` is sent for a choice: the default goes back to unset (null), so
 * the workspace follows the default if it changes; any other model is chosen by name. */
export function agentModelInput(choice: string, settings: WorkspaceSettings | null | undefined): { agent_model: string | null } {
  return { agent_model: choice === defaultAgentModel(settings) ? null : choice };
}
