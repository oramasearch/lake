/** One Ask Orama or Agent Factory conversation, shared by the Ask Orama page, the panel beside every
 * page and the Agent Factory (web_ui.cpp web_ui_ask, web_ui_factory).
 *
 * The conversation lives in the workspace Zetta. Sending stores the person's message and starts the
 * agent's turn (`send_ask_message`). A turn that runs on the bus is followed with `get_agent_task`
 * every 2 seconds, as the page's refresh did; once it is final, `settle_ask_turn` stores the
 * agent's answer once and the conversation is read again. */
import { type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { appendDictation } from "@/lib/dictation";
import { useCoarsePointer } from "@/lib/phoneSheet";
import { DictateButton, DictationError, RecordingBar, useRecorder } from "@/components/dictation/Dictate";
import { FromYourPhoneLink, FromYourPhonePanel } from "@/components/phone/FromYourPhone";
import { ApiError, act, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Badge, Button, ErrorBlock, Skeleton, cx } from "@/lib/ui";
import { isWorkspacePath } from "@/routes";
import { talkToAgents, type TalkToAgent } from "@/lib/agents";
import { Markdown, qs, secondsSince, spokenDuration, useWorkspace, useWq } from "@/pages/train/workspace";

export type AskKind = "ask" | "factory";

export type AskMessage = { id: string; role: string; content: string; payload?: Row | null; created_at?: string };
export type AgentTask = { task_id?: string; agent?: string; status?: string; message?: string; attempts?: number; created_at?: string; answer?: unknown };

const FINAL = (status: string | undefined) => !!status && status !== "pending" && status !== "running";

/** A path inside the app, under the current workspace when it is a workspace page. */
export function useAppPath() {
  const { workspaceId } = useSession();
  return (path: string) => {
    const bare = path.split(/[?#]/)[0];
    return workspaceId && isWorkspacePath(bare) ? `/w/${encodeURIComponent(workspaceId)}${path}` : path;
  };
}

/** The page a turn is asked about (lake#258): its path and each query value by name. */
export function pageOf(from: string): Row | undefined {
  if (!from) return undefined;
  const mark = from.indexOf("?");
  const page: Row = { path: mark < 0 ? from : from.slice(0, mark) };
  if (mark >= 0) {
    for (const [key, value] of new URLSearchParams(from.slice(mark + 1))) page[key] = value;
  }
  return page;
}

/** The Agent Library's display names (control::agent_display_name), by key. */
export function useAgentNames() {
  const library = useRead<{ agents?: Row[] }>("list_agent_library", undefined, { staleTime: 300_000 });
  return useMemo(() => {
    const names = new Map<string, string>();
    for (const agent of library.data?.agents ?? []) if (agent.key) names.set(String(agent.key), String(agent.name ?? agent.key));
    return { library, name: (key: string) => names.get(key) ?? key };
  }, [library]);
}

/** Who Ask Orama can talk to (lib/agents talkToAgents, control::talk_to_agents, lake#763), from
 * the Agent Library and the deferred /ask/agents list. */
export function useTalkTo(selected: string): TalkToAgent[] {
  const { library } = useAgentNames();
  const own = useWq<{ agents?: Row[] }>("list_agents", {}, { staleTime: 60_000 });
  return useMemo(() => talkToAgents(library.data?.agents, own.data?.agents, selected), [library.data, own.data, selected]);
}

/** The messages of a conversation, and the turn being followed. `onSettled` is called once the
 * followed turn's answer is stored. */
export function useConversation({
  kind,
  conversationId,
  taskId,
  onSettled,
}: {
  kind: AskKind;
  conversationId: string;
  taskId: string;
  onSettled: () => void;
}) {
  const { customerId, workspaceId } = useWorkspace();
  const ready = !!customerId && !!workspaceId;
  const messages = useRead<{ messages?: AskMessage[] }>(
    conversationId && ready ? "get_ask_conversation" : null,
    { customer_id: customerId, workspace_id: workspaceId, conversation_id: conversationId, limit: 200 },
    { staleTime: 0 },
  );
  const task = useRead<AgentTask>(
    taskId && ready ? "get_agent_task" : null,
    { customer_id: customerId, workspace_id: workspaceId, task_id: taskId },
    { refetchInterval: (q) => (FINAL((q.state.data as AgentTask | undefined)?.status) ? false : 2000), staleTime: 0 },
  );
  const [settleError, setSettleError] = useState<unknown>(null);
  const settling = useRef("");
  const status = task.data?.status;
  useEffect(() => {
    if (!taskId || !FINAL(status) || settling.current === taskId) {
      return;
    }
    settling.current = taskId;
    act<{ settled?: boolean }>("settle_ask_turn", { customer_id: customerId, workspace_id: workspaceId, conversation_id: conversationId, task_id: taskId, kind })
      .then((answer) => {
        if (answer.settled === false) {
          // Not final after all: follow it again.
          settling.current = "";
          void task.refetch();
          return;
        }
        onSettled();
      })
      .catch((error) => {
        setSettleError(error);
        settling.current = "";
      });
  }, [taskId, status, customerId, workspaceId, conversationId, kind, onSettled, task]);
  const working = !!taskId && (!task.data || !FINAL(task.data.status)) && !task.isError;
  return { messages, task, working, settleError, list: messages.data?.messages ?? [] };
}

/** Send a message: stores it and starts the turn. Answers the conversation and, for a turn on the
 * bus, its task. */
export async function sendMessage(input: { customerId: string; workspaceId: string; conversationId: string; kind: AskKind; agent: string; message: string; from: string; page?: Row }) {
  const body: Row = {
    customer_id: input.customerId,
    workspace_id: input.workspaceId,
    kind: input.kind,
    agent: input.agent,
    message: input.message,
  };
  if (input.conversationId) body.conversation_id = input.conversationId;
  // A page with something to ask about (the transcript page) sends its own context instead.
  const page = input.page ?? pageOf(input.from);
  if (page) body.page = page;
  return act<{ conversation_id: string; task_id?: string; message_id?: string; reply_id?: string }>("send_ask_message", body);
}

/* ------------------------------------------------------------------ rendering */

/** One message as lake rendered it: a person's text as text, an agent's as Markdown. */
export function MessageArticle({ person, who, content, children }: { person: boolean; who: string; content: string; children?: ReactNode }) {
  return (
    <article className={cx("ask-message", person ? "ask-person" : "ask-agent")}>
      <p className="eyebrow">{who}</p>
      {person ? (
        <p className="whitespace-pre-wrap">{content}</p>
      ) : (
        <div className="ask-reply">
          <Markdown>{content}</Markdown>
        </div>
      )}
      {children}
    </article>
  );
}

/** The working notice of an Ask turn (lake#451): who is working, for how long, what it last said. */
export function AskWorking({ task, fallbackAgent, name }: { task: AgentTask | undefined; fallbackAgent: string; name: (key: string) => string }) {
  const agent = name(task?.agent || fallbackAgent || "agent-ask-orama");
  const waiting = !task || task.status === "pending";
  const elapsed = secondsSince(task?.created_at ?? "");
  const progress = task?.message && task.message !== "Working on it" ? task.message : "";
  const attempts = task?.attempts ?? 1;
  return (
    <article className="ask-message ask-agent ask-working" role="status">
      <p className="eyebrow">{agent}</p>
      <p>
        <strong>{waiting ? `Waiting for ${agent} to start…` : `${agent} is working…`}</strong>
        {elapsed >= 0 ? ` · for ${spokenDuration(elapsed)}` : ""}
      </p>
      {progress && <p className="ask-progress">{progress}</p>}
      {attempts > 1 && <p className="sub">Attempt {attempts} of 3.</p>}
      <p className="sub">
        {elapsed >= 60 ? "Long turns are usual when it brings in another agent or reads a lot of data. " : ""}
        It keeps working if you leave; this page checks again every 2 seconds.
      </p>
    </article>
  );
}

/** The link to a turn's events in Streams (web_ui.cpp agent_events_link). */
export function TurnInStreams({ taskId }: { taskId: string }) {
  const { customerId } = useWorkspace();
  const path = useAppPath();
  if (!customerId || !taskId) return null;
  return (
    <p className="sub">
      <Link to={path(`/train/stream?${qs({ customer_id: customerId, zetta: "workspace", topic: "agent-events", task: taskId })}`)}>View this turn in Streams</Link>
    </p>
  );
}

/** The Ask Orama conversation: messages, delegations, proposals with Confirm, the working turn. */
export function AskMessages({
  conversationId,
  list,
  loading,
  error,
  working,
  task,
  agent,
}: {
  conversationId: string;
  list: AskMessage[];
  loading: boolean;
  error: unknown;
  working: boolean;
  task: AgentTask | undefined;
  agent: string;
}) {
  const { name } = useAgentNames();
  const { customerId, workspaceId } = useWorkspace();
  const path = useAppPath();
  const [confirming, setConfirming] = useState("");
  const [failures, setFailures] = useState<Record<string, string>>({});
  // Which proposals have already been confirmed, so a confirmed one is never offered again.
  const confirmed = useMemo(() => {
    const done = new Set<string>();
    for (const message of list) {
      const mark = message.payload?.confirmed;
      if (mark && typeof mark === "object") done.add(`${mark.message_id}/${mark.index}`);
    }
    return done;
  }, [list]);
  const confirm = async (messageId: string, index: number) => {
    const key = `${messageId}/${index}`;
    setConfirming(key);
    setFailures((f) => ({ ...f, [key]: "" }));
    try {
      const answer = await act<{ ok?: boolean; error?: string }>("confirm_ask_proposal", {
        customer_id: customerId,
        workspace_id: workspaceId,
        conversation_id: conversationId,
        message_id: messageId,
        index,
      });
      if (answer.ok === false && answer.error) setFailures((f) => ({ ...f, [key]: answer.error! }));
    } catch (e) {
      setFailures((f) => ({ ...f, [key]: e instanceof ApiError ? e.message : (e as Error).message }));
    } finally {
      setConfirming("");
    }
  };
  return (
    <section className="panel ask-conversation" aria-live="polite">
      {error ? <ErrorBlock error={error} what="this conversation" /> : null}
      {loading && <Skeleton lines={2} />}
      {!loading && !list.length && <p>Ask anything about your Lake: your repositories, projects, data, runs, models, services or agents.</p>}
      {list.map((message) => {
        const person = message.role === "user";
        const payload = message.payload && typeof message.payload === "object" ? message.payload : null;
        const who = person ? "You" : payload?.agent ? name(String(payload.agent)) : "Ask Orama";
        const delegations: Row[] = Array.isArray(payload?.delegations) ? payload!.delegations : [];
        const proposals: Row[] = Array.isArray(payload?.proposals) ? payload!.proposals : [];
        return (
          <MessageArticle key={message.id} person={person} who={who} content={message.content}>
            {delegations.map((delegation, i) => {
              const review = delegation?.answer && typeof delegation.answer === "object" ? String(delegation.answer.review_url ?? "") : "";
              return (
                <div key={`d${i}`}>
                  <p className="sub">
                    Asked {name(String(delegation?.agent ?? ""))}: {String(delegation?.request ?? "")}
                  </p>
                  {review.startsWith("/design/pipeline?") && (
                    <p>
                      <Button variant="primary" to={path(`${review}#proposal`)}>
                        Review the proposed change
                      </Button>
                    </p>
                  )}
                </div>
              );
            })}
            {proposals.map((proposal, index) => {
              const key = `${message.id}/${index}`;
              return (
                <div key={`p${index}`} className="panel ask-proposal">
                  <p>
                    <strong>Proposed:</strong> <code>{String(proposal?.capability ?? "")}</code>
                  </p>
                  <p>{String(proposal?.impact ?? "")}</p>
                  {confirmed.has(key) ? (
                    <p>
                      <Badge>Confirmed</Badge>
                    </p>
                  ) : (
                    <Button variant="primary" loading={confirming === key} disabled={!!confirming} onClick={() => void confirm(message.id, index)}>
                      Confirm
                    </Button>
                  )}
                  {failures[key] && (
                    <p className="error" role="alert">
                      {failures[key]}
                    </p>
                  )}
                </div>
              );
            })}
          </MessageArticle>
        );
      })}
      {working && <AskWorking task={task} fallbackAgent={agent} name={name} />}
    </section>
  );
}

/** *Talk to* (lake#763): every agent a person may talk to by its library name, the chosen one's
 * one-sentence purpose under the box, and where you meet it on expand. A library agent this Lake
 * does not run is listed and marked, and cannot be picked. */
function TalkToSelect({ agent, agents, onAgent }: { agent?: string; agents: TalkToAgent[]; onAgent: (agent: string) => void }) {
  const chosen = agents.find((a) => a.id === agent) ?? agents[0];
  return (
    <div className="talk-to stack gap-1">
      <label>
        Talk to
        <select name="agent" value={agent} onChange={(e) => onAgent(e.target.value)} aria-describedby={chosen?.purpose ? "talk-to-purpose" : undefined}>
          {agents.map((a) => (
            <option key={a.id} value={a.id} disabled={!a.running && a.id !== agent} title={a.purpose || undefined}>
              {a.running ? a.name : `${a.name} (not running on this Lake)`}
            </option>
          ))}
        </select>
      </label>
      {chosen?.purpose && (
        <p id="talk-to-purpose" className="muted m-0 text-[12.5px]">
          {chosen.purpose}
        </p>
      )}
      {chosen?.usedIn && (
        <details className="text-[12.5px] text-ink-3">
          <summary>Where you meet it</summary>
          {chosen.usedIn}
        </details>
      )}
    </div>
  );
}

/** The message box: who to talk to, the message, the microphone and Send. On a keyboard Enter
 * sends and Shift+Enter is a new line; on a touch screen Return makes a new line and Send sends, as
 * in messaging apps. Dictated text goes into the box to edit or send (components/dictation). */
export function AskComposer({
  agent,
  onAgent,
  agents,
  busy,
  error,
  onSend,
  label = "Message",
  placeholder = "What should I look at first?",
  compact,
  draft,
  draftKey,
  context,
}: {
  agent?: string;
  onAgent?: (agent: string) => void;
  agents?: TalkToAgent[];
  busy: boolean;
  error: unknown;
  onSend: (message: string) => Promise<boolean>;
  label?: string;
  placeholder?: string;
  compact?: boolean;
  /** Put in the box when it changes (Ask Orama opened about a transcript), or when `draftKey`
   * changes (the home's example questions, which may put the same question twice). */
  draft?: string;
  draftKey?: number;
  /** Shown above the box: what the next message is about. */
  context?: ReactNode;
}) {
  const [text, setText] = useState(draft ?? "");
  const box = useRef<HTMLTextAreaElement>(null);
  const touch = useCoarsePointer();
  const [phoneHelp, setPhoneHelp] = useState(false);
  const path = useAppPath();
  const rec = useRecorder({
    onText: (said) => {
      setText((prev) => appendDictation(prev, said));
      box.current?.focus();
    },
  });
  useEffect(() => {
    if (draft) {
      setText(draft);
      box.current?.focus();
    }
  }, [draft, draftKey]);
  const submit = async () => {
    const message = text.trim();
    if (!message || busy || rec.state !== "idle") return;
    if (await onSend(message)) setText("");
  };
  return (
    <form
      className="stack ask-composer"
      onSubmit={(e) => {
        e.preventDefault();
        void submit();
      }}
    >
      {error ? <ErrorBlock error={error} /> : null}
      {context}
      {agents && onAgent && <TalkToSelect agent={agent} agents={agents} onAgent={onAgent} />}
      <label>
        {label}
        <textarea
          ref={box}
          name="message"
          rows={compact ? 2 : 3}
          required
          placeholder={rec.state === "transcribing" ? "Transcribing…" : placeholder}
          value={text}
          enterKeyHint={touch ? "enter" : "send"}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && !e.shiftKey && !touch && !e.nativeEvent.isComposing) {
              e.preventDefault();
              void submit();
            }
          }}
        />
      </label>
      <DictationError rec={rec} />
      {phoneHelp && <FromYourPhonePanel where="ask" transcriptsPath={path("/transcripts")} onClose={() => setPhoneHelp(false)} className="max-h-[40vh] overflow-y-auto" />}
      <div className="flex min-w-0 items-center gap-1.5">
        {rec.state === "recording" ? (
          <RecordingBar rec={rec} />
        ) : (
          <>
            <DictateButton rec={rec} title="Dictate: speak your message" className="border border-rule" />
            <FromYourPhoneLink open={phoneHelp} onToggle={() => setPhoneHelp((v) => !v)} className="ml-1" />
            <span className="flex-1" />
            <Button variant="primary" type="submit" loading={busy} disabled={rec.state !== "idle"} className="ask-send">
              Send
            </Button>
          </>
        )}
      </div>
    </form>
  );
}
