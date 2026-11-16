// SPDX-License-Identifier: MIT

/** A conversation with the Application Agent that changes one App (lake#1240,
 * docs/ui/apps.md#changing-an-app-in-a-conversation): the thread `get_app_conversation` answers, what
 * each turn shows, the proposal Confirm applies, and when a confirmed change has been applied. Pure,
 * so `npm test` covers it (tests/app-conversation.test.ts). */

type Json = Record<string, unknown>;

/** One turn of the thread: a `get_agent_task` view, plus what the person wrote (`request`) and
 * whether it confirmed a proposal (`confirmation`). */
export type Turn = {
  task_id: string;
  status?: string;
  kind?: string;
  message?: string;
  request?: string;
  confirmation?: boolean;
  conversation_id?: string;
  created_at?: string;
  finished_at?: string;
  answer?: Json | null;
  proposal_sha256?: string;
};

export type Conversation = { app_id: string; conversation_id: string; turns: Turn[]; truncated?: boolean };

function object(value: unknown): Json {
  return value && typeof value === "object" && !Array.isArray(value) ? (value as Json) : {};
}

/** The structured result the agent answered (`answer.result`). */
export function turnResult(turn: Turn | undefined): Json {
  return object(object(turn?.answer).result);
}

/** The agent's reply: `answer.reply`, else the result's own reply, else its error, else the task's
 * message. */
export function turnReply(turn: Turn | undefined): string {
  const answer = object(turn?.answer);
  if (typeof answer.reply === "string" && answer.reply) return answer.reply;
  const result = turnResult(turn);
  if (typeof result.reply === "string" && result.reply) return result.reply;
  if (typeof answer.error === "string" && answer.error) return answer.error;
  return turn?.message ?? "";
}

/** A turn is working while it is pending or running. */
export function isWorking(turn: Turn | undefined): boolean {
  return !!turn && (turn.status === "pending" || turn.status === "running" || !turn.status);
}

/** Whether any turn of the thread is still working (the page reads it again every 2 seconds). */
export function threadWorking(turns: Turn[]): boolean {
  return turns.some(isWorking);
}

/** Whether a turn's answer is a proposal the person can confirm: a finished change of an App with
 * its files, not itself a confirmation. */
export function isProposal(turn: Turn | undefined): boolean {
  if (!turn || turn.status !== "succeeded" || turn.confirmation) return false;
  const result = turnResult(turn);
  const app = result.app;
  return result.kind === "proposal" && result.confirm === true && !!app && typeof app === "object" && Array.isArray(result.files);
}

/** The proposal Confirm applies: the newest turn's, while nothing came after it. An older proposal
 * is history (the person moved on), so it is shown but cannot be confirmed. */
export function confirmable(turns: Turn[]): Turn | null {
  const last = turns[turns.length - 1];
  return isProposal(last) ? last : null;
}

/** `change_app`'s input for the person's next message; Lake continues their own conversation. */
export function sendInput(appId: string, customerId: string, workspaceId: string | null, message: string) {
  return { customer_id: customerId, workspace_id: workspaceId, app_id: appId, message: message.trim() };
}

/** `agent_turn`'s input confirming `turn`'s proposal: the existing confirmation path (kind `change`,
 * the proposal's task and digest, the proposal exactly as shown), in the same conversation. */
export function confirmInput(turn: Turn, appId: string, customerId: string, workspaceId: string | null) {
  return {
    customer_id: customerId,
    workspace_id: workspaceId,
    agent: "agent-application",
    kind: "change",
    message: "Apply the reviewed change.",
    conversation_id: turn.conversation_id,
    proposal_task_id: turn.task_id,
    proposal_sha256: turn.proposal_sha256 ?? "",
    context: { app_id: appId, confirmed_proposal: turnResult(turn) },
  };
}

/** One bubble of the thread. */
export type Entry =
  | { id: string; role: "you"; text: string; confirmation: boolean }
  | { id: string; role: "agent"; state: "working" | "failed" | "question" | "proposal" | "applied" | "reply"; text: string; diff?: string; files?: number; current?: boolean; confirmed?: boolean };

/** The thread as bubbles, oldest first: what the person wrote, then what the agent answered. */
export function entries(turns: Turn[]): Entry[] {
  const out: Entry[] = [];
  const current = confirmable(turns);
  turns.forEach((turn, index) => {
    out.push({ id: `${turn.task_id}:you`, role: "you", text: turn.confirmation ? "Confirmed the proposed change." : (turn.request ?? ""), confirmation: !!turn.confirmation });
    const result = turnResult(turn);
    const text = turnReply(turn);
    const id = `${turn.task_id}:agent`;
    if (isWorking(turn)) {
      out.push({ id, role: "agent", state: "working", text: turn.confirmation ? "Committing the change and redeploying…" : "Thinking…" });
    } else if (turn.status === "failed") {
      out.push({ id, role: "agent", state: "failed", text: text || "The Application Agent could not finish this turn." });
    } else if (isProposal(turn)) {
      const files = Array.isArray(result.files) ? result.files.length : 0;
      // The turn after it confirmed it (the conversation's confirmation always follows its proposal),
      // so it is applied below rather than replaced (lake#1259).
      const confirmed = !!turns[index + 1]?.confirmation;
      out.push({ id, role: "agent", state: "proposal", text, diff: typeof result.diff === "string" ? result.diff : "", files, current: current?.task_id === turn.task_id, confirmed });
    } else if (turn.confirmation) {
      out.push({ id, role: "agent", state: "applied", text: text || "Committed and redeployed.", diff: typeof result.diff === "string" ? result.diff : undefined });
    } else {
      out.push({ id, role: "agent", state: result.kind === "question" ? "question" : "reply", text: text || "The Application Agent did not answer." });
    }
  });
  return out;
}

/** The confirmation turns that finished successfully since `before` was read: a confirmed change the
 * agent has committed and deployed, whose rollout the page now follows. Nothing on the first read. */
export function newlyApplied(before: Turn[] | undefined, after: Turn[]): Turn[] {
  if (!before) return [];
  const was = new Map(before.map((turn) => [turn.task_id, turn.status]));
  return after.filter((turn) => turn.confirmation && turn.status === "succeeded" && was.get(turn.task_id) !== "succeeded");
}
