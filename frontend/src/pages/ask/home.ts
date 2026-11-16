// SPDX-License-Identifier: MIT

/** The home and Ask Orama's page, as pure rules so `npm test` runs them (tests/home.test.ts): what
 * `/` shows each viewer (Quickstart for a signed-in person since lake#1128; Ask Orama was the home
 * from lake#600), where an old home link with a conversation, an `/ask` link or an old Command
 * Center link goes, what a
 * link's query hands the shared conversation, and the starting questions an empty conversation
 * offers, drawn from the workspace. No imports. */

type Viewer = { signed_in: boolean; administrator?: boolean };

/** The conversation the panel and the home share, per workspace (pages/ask/thread.ts). */
export type ThreadState = { conversation: string; task: string; agent: string };
export const ASK_AGENT = "agent-ask-orama";
export const EMPTY_THREAD: ThreadState = { conversation: "", task: "", agent: ASK_AGENT };

/** Whether this viewer has Ask Orama's page and the Quickstart home: a signed-in person. The
 * operator and visitors keep the landing. */
export function asksAtHome(viewer: Viewer): boolean {
  return viewer.signed_in && !viewer.administrator;
}

/** The query keys that hand Ask Orama a conversation: a home link with one of these was an Ask Orama
 * link while Ask Orama was the home (lake#600), and opens Ask Orama's page now. */
const ASK_KEYS = ["conversation", "task", "agent", "from"];

/** What `/` shows: the landing, Quickstart (lake#1128), or a redirect: an old Command Center link
 * (`/?ws=`) to the Dashboard, an old Ask Orama home link (`/?conversation=`, `/?agent=`,
 * `/?from=`) to `/ask`, each with its query. */
export function homeView(viewer: Viewer, search: string): { view: "landing" } | { view: "quickstart" } | { view: "redirect"; to: string } {
  if (!asksAtHome(viewer)) return { view: "landing" };
  const params = new URLSearchParams(search);
  const query = search.startsWith("?") ? search : `?${search}`;
  if (params.has("ws")) return { view: "redirect", to: `/dashboard${query}` };
  if (ASK_KEYS.some((key) => params.has(key))) return { view: "redirect", to: askLinkTarget(search) };
  return { view: "quickstart" };
}

/** Whether a pathname is Ask Orama's page for this viewer: `/ask`, where the conversation is the
 * page, so the side panel never opens over it. */
export function isAskHome(pathname: string, viewer: Viewer): boolean {
  return asksAtHome(viewer) && pathname === "/ask";
}

/** Where an Ask Orama link (`/w/<id>/ask`, an old home link with a conversation) goes: `/ask`, with
 * its query. `customer_id` is dropped: the page works in the session's workspace. */
export function askLinkTarget(search: string): string {
  const params = new URLSearchParams(search);
  params.delete("customer_id");
  const rest = params.toString();
  return rest ? `/ask?${rest}` : "/ask";
}

/** What a home link's query asks of the shared conversation: `conversation` (with `task` and
 * `agent`) opens that conversation; `agent` alone starts a new conversation with that agent
 * ("Talk to this agent in Ask Orama"); neither leaves the conversation as it is. `from` stays: the
 * page the conversation is about. Answers what to adopt (or null) and the query to keep. */
export function linkThread(search: string): { adopt: Partial<ThreadState> | null; keep: string } {
  const params = new URLSearchParams(search);
  const conversation = params.get("conversation") ?? "";
  const task = params.get("task") ?? "";
  const agent = params.get("agent") ?? "";
  let adopt: Partial<ThreadState> | null = null;
  if (conversation) adopt = { conversation, task, ...(agent ? { agent } : {}) };
  else if (agent) adopt = { conversation: "", task: "", agent };
  const from = params.get("from") ?? "";
  const keep = from ? `?${new URLSearchParams({ from }).toString()}` : "";
  return { adopt, keep };
}

/** The side panel's "Open as a page": Ask Orama's page, about the page the panel was opened from. */
export function homeLink(from: string): string {
  return from && from !== "/" && from !== "/ask" ? `/ask?${new URLSearchParams({ from }).toString()}` : "/ask";
}

/** One run, as list_runs names it. */
type Run = { status?: unknown; state?: unknown; pipeline_name?: unknown; graph?: unknown; project_name?: unknown; project?: unknown };

function runName(run: Run): string {
  return String(run.pipeline_name || run.graph || run.project_name || run.project || "");
}

function runStatus(run: Run): string {
  return String(run.status ?? run.state ?? "").toLowerCase();
}

/** The questions an empty conversation offers, from what the workspace holds: a failed run first,
 * then its datasets, projects and latest run; a workspace with none of these is offered the first
 * steps. At most four, never two alike. */
export function starterQuestions(state: { datasets?: string[]; projects?: string[]; runs?: Run[] }): string[] {
  const datasets = (state.datasets ?? []).filter(Boolean);
  const projects = (state.projects ?? []).filter(Boolean);
  const runs = (state.runs ?? []).filter((r) => r && typeof r === "object");
  const out: string[] = [];
  const failed = runs.find((r) => ["failed", "error"].includes(runStatus(r)) && runName(r));
  if (failed) out.push(`Why did the latest run of ${runName(failed)} fail?`);
  if (datasets.length) out.push(`What is in the ${datasets[0]} dataset, and what could I predict from it?`);
  if (projects.length) out.push(`Where does the ${projects[0]} project stand, and what should I do next?`);
  const finished = runs.find((r) => ["succeeded", "success", "completed", "done"].includes(runStatus(r)) && runName(r));
  if (finished) out.push(`What did the latest run of ${runName(finished)} find?`);
  if (datasets.length > 1) out.push(`How do ${datasets[0]} and ${datasets[1]} relate?`);
  if (!datasets.length) out.push("How do I load my first dataset?");
  if (!projects.length) out.push("Help me turn a business question into a project.");
  out.push("What is in this workspace, and what should I look at first?");
  out.push("What can Lake do with my data?");
  return [...new Set(out)].slice(0, 4);
}

/** The empty home's heading: the person's first name when lake knows one (never an email). */
export function greeting(displayName: string | undefined): string {
  const first = (displayName ?? "").trim().split(/\s+/)[0] ?? "";
  if (!first || first.includes("@")) return "What would you like to know?";
  return `Hello, ${first}. What would you like to know?`;
}

/** Where a sign-in lands when nothing names a page: a signed-in person's home, Quickstart
 * (lake#1128); the operator, Repositories as before (lake's Clerk and OAuth routes answer the same,
 * lake#600). */
export function landingAfterSignIn(administrator: boolean): string {
  return administrator ? "/repositories" : "/";
}

/** A confirm that did not work, as Lake stored it: its reason, and whether pressing the button again
 * could work. */
export type ConfirmRefusal = { reason: string; retry: boolean };

/** What each confirmed proposal came to, from the outcome messages Lake stored in the conversation
 * (`payload.confirmed` names the proposal as `message_id/index`). Only a confirm that worked is
 * Confirmed and never offered again; one that did not work (`payload.ok` false) keeps its reason,
 * so a failed confirm never reads as done (lake#794). `retry` is Lake's own `payload.retry`: true
 * only for a refusal that may pass later with the same input (the workspace not reachable, a busy
 * resource); a refusal of the input itself fails the same way every time, so the page offers no
 * Try again and the reason says to ask Ask Orama again. An outcome stored before Lake recorded
 * `retry` keeps Try again. A later success wins. */
export function confirmOutcomes(
  list: { content?: unknown; payload?: { confirmed?: unknown; ok?: unknown; retry?: unknown } | null }[],
): { confirmed: Set<string>; refused: Record<string, ConfirmRefusal> } {
  const confirmed = new Set<string>();
  const refused: Record<string, ConfirmRefusal> = {};
  for (const message of list) {
    const mark = message.payload?.confirmed as { message_id?: unknown; index?: unknown } | undefined;
    if (!mark || typeof mark !== "object") continue;
    const key = `${mark.message_id}/${mark.index}`;
    if (message.payload?.ok === false) {
      if (!confirmed.has(key)) refused[key] = { reason: String(message.content ?? ""), retry: message.payload?.retry !== false };
    } else {
      confirmed.add(key);
      delete refused[key];
    }
  }
  return { confirmed, refused };
}

/** One field of the call a proposal makes, as Lake states it: the capability schema's description
 * and the exact value. */
export type ProposalCallField = { name: string; description: string; value: unknown };

/** The exact call Confirm runs for one stored proposal (lake#942), Lake's own statement of it from
 * get_ask_conversation's `proposal_calls`, never the model's `impact`: the capability, its registry
 * title and kind, every input field, and the digest Confirm sends back so only this call runs. */
export type ProposalCall = { capability: string; title: string; kind: string; fields: ProposalCallField[]; input_sha256: string };

/** Lake's call for proposal `index` of `message`, or null when Lake states none or states it in a
 * shape this page does not know. A null call is shown as not confirmable: Confirm is never offered
 * for a proposal whose exact arguments the person cannot see. */
export function proposalCall(message: { proposal_calls?: unknown }, index: number): ProposalCall | null {
  const calls = message.proposal_calls;
  const call = Array.isArray(calls) ? (calls[index] as Partial<ProposalCall> | null | undefined) : null;
  if (!call || typeof call !== "object") return null;
  if (typeof call.capability !== "string" || !call.capability) return null;
  if (typeof call.input_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(call.input_sha256)) return null;
  if (!Array.isArray(call.fields)) return null;
  const fields: ProposalCallField[] = [];
  for (const field of call.fields as unknown[]) {
    if (!field || typeof field !== "object" || !("value" in field)) return null;
    const { name, description, value } = field as { name?: unknown; description?: unknown; value: unknown };
    if (typeof name !== "string" || !name) return null;
    fields.push({ name, description: typeof description === "string" ? description : "", value });
  }
  return {
    capability: call.capability,
    title: typeof call.title === "string" ? call.title : "",
    kind: typeof call.kind === "string" ? call.kind : "",
    fields,
    input_sha256: call.input_sha256,
  };
}

/** `text` with every control, format, bidirectional or non-space whitespace character (a
 * right-to-left override, a zero-width space) written as its `\u{…}` code point, so nothing in it
 * can hide or reorder what is shown (lake#942). Spaces and line breaks stay. */
export function visibleText(text: string): string {
  return text.replace(/[\p{C}\p{Z}]/gu, (c) => (c === " " || c === "\n" ? c : `\\u{${c.codePointAt(0)!.toString(16)}}`));
}

/** A field's value as Confirm shows it (lake#942): JSON, so a string keeps its quotes and its
 * leading or trailing spaces show and an object or array is laid out, then visibleText. */
export function shownValue(value: unknown): string {
  return visibleText(JSON.stringify(value, null, 2) ?? String(value));
}
