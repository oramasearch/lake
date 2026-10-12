/** The writes Design → Agents and Design → Agent make (lake#653), through Lake's named engine
 * capabilities (`create_agent`, `update_agent`, `delete_agent`, lake#650) and Lake's own
 * schedules and the streams that start an agent (`set_agent_streams`, lake#677). */
import { ApiError, act, read, type Row } from "@/lib/api";
import { agentInput, formProblem, streamsToSet, text, type AgentForm } from "@/lib/agents";
import type { Scope } from "./api";

/** A refusal the form makes before sending, on the field at fault. */
function refuse(form: AgentForm) {
  const problem = formProblem(form);
  if (problem) throw new ApiError(400, "invalid_input", problem.message, `/${problem.field}`);
}

/** Create an agent from the form and answer its id. A frontier-model agent with a schedule gets it
 * as Lake's schedule too, since Lake fires it (docs/api/agent-launches.md), and its streams as
 * Lake's stream triggers; a schedule or streams that are refused are said, and the agent is still
 * created. */
export async function createAgent(
  scope: Scope,
  form: AgentForm,
  frontierModel?: string,
): Promise<{ id: string; scheduleError?: string; streamsError?: string }> {
  refuse(form);
  const input = agentInput(form, { frontierModel });
  const created = await act<Row>("create_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...input });
  const id = text(created, "id");
  const made: { id: string; scheduleError?: string; streamsError?: string } = { id };
  if (id && input.runtime === "python" && text(input, "trigger")) {
    try {
      await act("schedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id, trigger: text(input, "trigger") });
    } catch (error) {
      made.scheduleError = (error as Error).message;
    }
  }
  const streams = streamsToSet(form);
  if (id && streams) {
    try {
      await setAgentStreams(scope, id, streams);
    } catch (error) {
      made.streamsError = (error as Error).message;
    }
  }
  return made;
}

/** The streams whose new messages start an agent: the whole set, as Lake's set_agent_streams takes
 * it. */
export async function setAgentStreams(scope: Scope, agentId: string, streams: string[]): Promise<Row> {
  return act<Row>("set_agent_streams", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: agentId, streams });
}

/** Save an edit of one of the workspace's own agents: a new revision of its definition, and its
 * streams when they changed (`before`, what list_agent_streams said). Streams that are refused are
 * said; the revision is still saved. */
export async function updateAgent(
  scope: Scope,
  agent: Row,
  form: AgentForm,
  frontierModel?: string,
  before: string[] = [],
): Promise<{ agent: Row; streamsError?: string }> {
  refuse(form);
  const input = agentInput(form, { frontierModel, existing: agent });
  const saved = await act<Row>("update_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(agent, "id"), ...input });
  const streams = streamsToSet(form, before);
  if (streams) {
    try {
      await setAgentStreams(scope, text(agent, "id"), streams);
    } catch (error) {
      return { agent: saved, streamsError: (error as Error).message };
    }
  }
  return { agent: saved };
}

/** Restore a revision: update_agent with the definition it set, which Lake keeps as a new
 * revision. */
export async function restoreRevision(scope: Scope, agentId: string, definition: Row): Promise<Row> {
  return act<Row>("update_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: agentId, ...definition });
}

/** Delete an agent and every schedule Lake keeps for it, so no CronJob outlives its agent. Lake
 * removes the streams that start it with the agent itself (delete_agent, lake#677). */
export async function deleteAgent(scope: Scope, agentId: string): Promise<void> {
  await act("delete_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: agentId });
  const listed = await read<{ schedules?: Row[] }>("list_agent_schedules", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: agentId }).catch(() => ({ schedules: [] as Row[] }));
  for (const schedule of listed.schedules ?? []) {
    await act("unschedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, schedule_id: text(schedule, "id") }).catch(() => undefined);
  }
}
