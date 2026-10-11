/** The writes Design → Agents and Design → Agent make (lake#653), through Lake's named engine
 * capabilities (`create_agent`, `update_agent`, `delete_agent`, lake#650) and Lake's own
 * schedules. */
import { ApiError, act, read, type Row } from "@/lib/api";
import { agentInput, formProblem, text, type AgentForm } from "@/lib/agents";
import type { Scope } from "./api";

/** A refusal the form makes before sending, on the field at fault. */
function refuse(form: AgentForm) {
  const problem = formProblem(form);
  if (problem) throw new ApiError(400, "invalid_input", problem.message, `/${problem.field}`);
}

/** Create an agent from the form and answer its id. A frontier-model agent with a schedule gets it
 * as Lake's schedule too, since Lake fires it (docs/api/agent-launches.md); a schedule that is
 * refused is said, and the agent is still created. */
export async function createAgent(scope: Scope, form: AgentForm, frontierModel?: string): Promise<{ id: string; scheduleError?: string }> {
  refuse(form);
  const input = agentInput(form, { frontierModel });
  const created = await act<Row>("create_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...input });
  const id = text(created, "id");
  if (id && input.runtime === "python" && text(input, "trigger")) {
    try {
      await act("schedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: id, trigger: text(input, "trigger") });
    } catch (error) {
      return { id, scheduleError: (error as Error).message };
    }
  }
  return { id };
}

/** Save an edit of one of the workspace's own agents: a new revision of its definition. */
export async function updateAgent(scope: Scope, agent: Row, form: AgentForm, frontierModel?: string): Promise<Row> {
  refuse(form);
  const input = agentInput(form, { frontierModel, existing: agent });
  return act<Row>("update_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(agent, "id"), ...input });
}

/** Restore a revision: update_agent with the definition it set, which Lake keeps as a new
 * revision. */
export async function restoreRevision(scope: Scope, agentId: string, definition: Row): Promise<Row> {
  return act<Row>("update_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: agentId, ...definition });
}

/** Delete an agent and every schedule Lake keeps for it, so no CronJob outlives its agent. */
export async function deleteAgent(scope: Scope, agentId: string): Promise<void> {
  await act("delete_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: agentId });
  const listed = await read<{ schedules?: Row[] }>("list_agent_schedules", { customer_id: scope.customerId, workspace_id: scope.workspaceId, agent_id: agentId }).catch(() => ({ schedules: [] as Row[] }));
  for (const schedule of listed.schedules ?? []) {
    await act("unschedule_agent", { customer_id: scope.customerId, workspace_id: scope.workspaceId, schedule_id: text(schedule, "id") }).catch(() => undefined);
  }
}
