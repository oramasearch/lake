/** Changing an App in a conversation (lake#1240): the thread keeps its turns, only the newest
 * proposal can be confirmed, a confirm is the existing agent_turn confirmation that commits and
 * redeploys, and the page learns when it was applied. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { confirmable, confirmInput, entries, isProposal, newlyApplied, sendInput, threadWorking, turnReply, type Turn } from "../src/pages/apps/conversation.ts";

const proposal = { kind: "proposal", confirm: true, turn: "change", diff: "+chart", files: [{ path: "app.py" }], app: { id: "app-1", name: "reports" } };
const asked: Turn = { task_id: "t1", status: "succeeded", request: "Add a chart.", conversation_id: "c", answer: { ok: true, reply: "By region or country?", result: { kind: "question" } } };
const proposed: Turn = { task_id: "t2", status: "succeeded", request: "By region.", conversation_id: "c", answer: { ok: true, reply: "Here it is.", result: proposal }, proposal_sha256: "sha" };
const applying: Turn = { task_id: "t3", status: "running", request: "Apply the reviewed change.", confirmation: true, conversation_id: "c" };
const applied: Turn = { ...applying, status: "succeeded", answer: { ok: true, reply: "Committed and redeployed." } };

test("the thread keeps every turn, the person's and the agent's, in order", () => {
  const thread = entries([asked, proposed, applied, { task_id: "t4", status: "pending", request: "Now by plan.", conversation_id: "c" }]);
  assert.deepEqual(
    thread.map((entry) => (entry.role === "you" ? `you:${entry.text}` : `agent:${entry.state}`)),
    ["you:Add a chart.", "agent:question", "you:By region.", "agent:proposal", "you:Confirmed the proposed change.", "agent:applied", "you:Now by plan.", "agent:working"],
  );
  const shown = thread[3];
  assert.ok(shown.role === "agent" && shown.diff === "+chart" && shown.files === 1);
  assert.ok(shown.role === "agent" && shown.current === false, "a proposal the person moved past is history");
});

test("a proposal the person confirmed says so, not that a later message replaced it (lake#1259)", () => {
  const confirmedThread = entries([asked, proposed, applied]);
  const confirmedProposal = confirmedThread[3];
  assert.ok(confirmedProposal.role === "agent" && confirmedProposal.state === "proposal" && confirmedProposal.confirmed === true && confirmedProposal.current === false);
  const replaced = entries([asked, proposed, { task_id: "t4", status: "succeeded", request: "No, by plan.", conversation_id: "c", answer: { ok: true, reply: "By plan, then?", result: { kind: "question" } } }])[3];
  assert.ok(replaced.role === "agent" && replaced.confirmed === false, "a message that is not a confirmation replaces the proposal");
  const view = readFileSync(new URL("../src/pages/apps/AppConversation.tsx", import.meta.url), "utf8");
  assert.match(view, /entry\.confirmed \? " You confirmed it; what happened is below\." : " A later message replaced this proposal\."/);
});

test("only the newest proposal can be confirmed, and never twice", () => {
  assert.equal(confirmable([asked, proposed])?.task_id, "t2");
  assert.equal(confirmable([asked, proposed, applying]), null);
  assert.equal(confirmable([asked]), null);
  assert.equal(confirmable([]), null);
  assert.ok(!isProposal({ ...proposed, status: "running" }));
  assert.ok(!isProposal({ ...proposed, answer: { result: { ...proposal, confirm: false } } }));
  const current = entries([asked, proposed])[3];
  assert.ok(current.role === "agent" && current.current === true);
});

test("a confirm is the existing agent_turn confirmation, in the same conversation", () => {
  assert.deepEqual(confirmInput(proposed, "app-1", "cust", "ws"), {
    customer_id: "cust",
    workspace_id: "ws",
    agent: "agent-application",
    kind: "change",
    message: "Apply the reviewed change.",
    conversation_id: "c",
    proposal_task_id: "t2",
    proposal_sha256: "sha",
    context: { app_id: "app-1", confirmed_proposal: proposal },
  });
  assert.deepEqual(sendInput("app-1", "cust", "ws", "  Now by plan. "), { customer_id: "cust", workspace_id: "ws", app_id: "app-1", message: "Now by plan." });
});

test("the page learns once that a confirmed change was committed and redeployed", () => {
  assert.deepEqual(newlyApplied(undefined, [asked, proposed, applied]), [], "nothing on the first read");
  assert.deepEqual(newlyApplied([asked, proposed, applying], [asked, proposed, applied]).map((turn) => turn.task_id), ["t3"]);
  assert.deepEqual(newlyApplied([asked, proposed], [asked, proposed, applied]).map((turn) => turn.task_id), ["t3"], "even when it finished between two reads");
  assert.deepEqual(newlyApplied([asked, proposed, applied], [asked, proposed, applied]), [], "only once");
  assert.deepEqual(newlyApplied([asked, proposed, applying], [asked, proposed, { ...applying, status: "failed" }]), []);
});

test("the thread is read again while a turn works", () => {
  assert.ok(threadWorking([asked, applying]));
  assert.ok(!threadWorking([asked, applied]));
  assert.equal(turnReply({ task_id: "x", status: "failed", answer: { ok: false, error: "model unavailable" } }), "model unavailable");
});

test("the app page's agent card holds the conversation, which reads get_app_conversation", () => {
  const card = readFileSync(new URL("../src/pages/apps/repair.tsx", import.meta.url), "utf8");
  assert.match(card, /<AppConversation /);
  const component = readFileSync(new URL("../src/pages/apps/AppConversation.tsx", import.meta.url), "utf8");
  assert.match(component, /"get_app_conversation"/);
  assert.match(component, /act\("change_app", sendInput\(/);
  assert.match(component, /act\("agent_turn", confirmInput\(/);
  // The stored proposal is read again and compared before anything is confirmed.
  assert.match(component, /fetchAgentTask\(proposal\.task_id/);
});
