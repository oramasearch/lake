/** Design → Agents → Agent Factory, one of the two ways an agent is designed (lake#653; web_ui.cpp web_ui_factory, docs/api/agent-factory.md): a
 * conversation with the Agent Creator Agent beside its proposal, and Approve and create. Nothing is created
 * until the person approves; Approve sends the SHA-256 of the proposal they saw, which the server
 * checks against the proposal stored in the conversation. */
import { useCallback, useEffect, useMemo, useRef } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { ArrowLeft, Check, FileCode2, Sparkles } from "lucide-react";
import { act, type Row } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Card, Chip, ErrorBlock, KeyValue, Page, Skeleton, Split, Status, cx } from "@/lib/ui";
import { AskComposer, TurnInStreams, sendMessage, useConversation } from "@/pages/ask/conversation";
import { proposalDigest } from "@/pages/ask/digest";
import { skillLabels } from "./agents";
import { Markdown, qs, text, useAsync, useWorkspace, useWsPath } from "./workspace";

/** What a person might ask for first; each is sent as it reads. */
const EXAMPLES = [
  "An agent that flags renewals at risk every Monday",
  "An agent that answers questions about one dataset",
  "An agent that summarizes each finished run's report",
];

export default function FactoryPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const ws = useWsPath();
  const toast = useToast();
  const { customerId, workspaceId } = useWorkspace();
  const conversationId = params.get("conversation") ?? "";
  const taskId = params.get("task") ?? "";
  const query = qs({ customer_id: customerId });
  useTrail([{ label: "Design", to: ws("/design/projects") }, { label: "Agents", to: ws(`/design/agents?${query}`) }, { label: "Agent Factory" }]);

  const go = useCallback(
    (conversation: string, task: string, replace = true) => {
      navigate(ws(`/design/agents/new?${qs({ customer_id: params.get("customer_id"), conversation, task })}`), { replace });
    },
    [navigate, ws, params],
  );
  const onSettled = useCallback(() => go(conversationId, ""), [go, conversationId]);
  const conversation = useConversation({ kind: "factory", conversationId, taskId, onSettled });
  const send = useAsync();
  const approve = useAsync();

  // Agent Creator Agent's latest structured answer: the last agent message that carries one.
  const latest = useMemo(() => {
    let found: { answer: Row; messageId: string; digest?: string } | null = null;
    for (const message of conversation.list) {
      const answer = message.payload && typeof message.payload === "object" ? message.payload.answer : undefined;
      if (message.role === "assistant" && answer !== undefined) found = { answer: answer && typeof answer === "object" ? answer : {}, messageId: message.id, digest: message.payload?.proposal_sha256 };
    }
    return found;
  }, [conversation.list]);
  const proposing = latest?.answer.kind === "proposal";

  const ask = (message: string) =>
    send.run(async () => {
      const answer = await sendMessage({ customerId, workspaceId, conversationId, kind: "factory", agent: "agent-creator", message, from: "" });
      go(answer.conversation_id, answer.task_id ?? "", false);
    });

  // The conversation follows its newest message.
  const scroller = useRef<HTMLDivElement>(null);
  const count = conversation.list.length;
  useEffect(() => {
    const box = scroller.current;
    if (box) box.scrollTop = box.scrollHeight;
  }, [count, conversation.working]);

  return (
    <Page
      eyebrow="Design"
      title="Agent Factory"
      sub="Tell the Agent Creator Agent what you want an agent for. It asks what it must, then proposes the agent. Nothing is created until you approve the proposal."
      actions={
        <Button to={ws(`/design/agents?${query}`)}>
          <ArrowLeft size={14} aria-hidden /> Back to agents
        </Button>
      }
    >
      <Split
        side={
          proposing && latest ? (
            <Proposal
              answer={latest.answer}
              busy={approve.busy}
              error={approve.error}
              onApprove={() =>
                void approve.run(async () => {
                  const created = await act<{ agent_id?: string; name?: string; schedule_error?: string; check_launch_id?: string }>("approve_agent_proposal", {
                    customer_id: customerId,
                    workspace_id: workspaceId,
                    conversation_id: conversationId,
                    message_id: latest.messageId,
                    // The digest lake stored with the proposal; computed here only for a message stored before it did.
                    proposal_sha256: latest.digest ?? proposalDigest(latest.answer.proposal ?? null),
                  });
                  let words = `Created ${created.name ?? "the agent"}.`;
                  if (created.schedule_error) words += ` Its trigger was not scheduled: ${created.schedule_error}`;
                  if (created.check_launch_id) words += " A first check run has started.";
                  toast(words, created.schedule_error ? "bad" : "ok");
                  navigate(created.agent_id ? ws(`/design/agent?${qs({ customer_id: customerId, id: created.agent_id })}`) : ws(`/design/agents?${query}`));
                })
              }
            />
          ) : (
            <Card label="How It Works">
              <ol className="m-0 space-y-2 pl-5 text-[13px] text-ink-2">
                <li>Describe the agent: what it is for, what it may touch, and what done looks like.</li>
                <li>The Agent Creator Agent asks what it must, then proposes the agent here.</li>
                <li>Read the proposal and approve it. Nothing is created before you do.</li>
              </ol>
            </Card>
          )
        }
      >
        <section aria-label="Conversation with the Agent Creator Agent" className="flex min-h-[520px] flex-col rounded-[6px] border border-hairline bg-surface max-md:min-h-[420px]">
          <div className="flex flex-wrap items-center gap-2 border-b border-hairline px-4 py-2.5">
            <Sparkles size={15} className="text-ai" aria-hidden />
            <span className="label m-0">Agent Creator Agent</span>
            <span className="ml-auto">
              {conversation.working ? <Status value="working" tone="ai" /> : proposing ? <Status value="proposal ready" tone="success" /> : conversationId ? <Status value="waiting for you" tone="neutral" /> : null}
            </span>
          </div>
          <div ref={scroller} className="max-h-[62vh] min-h-0 flex-1 space-y-4 overflow-y-auto overscroll-contain px-3 py-4 md:px-5" aria-live="polite">
            {conversation.messages.isError ? <ErrorBlock error={conversation.messages.error} what="this conversation" /> : null}
            {conversation.settleError ? <ErrorBlock error={conversation.settleError} /> : null}
            {conversation.messages.isLoading && <Skeleton lines={2} />}
            {!conversation.messages.isLoading && !conversation.list.length && (
              <div className="mx-auto max-w-xl py-6 text-center">
                <p className="text-[13.5px] text-ink-2">Describe the agent: what it is for, what it may touch, and what done looks like.</p>
                <div className="mt-4 space-y-1.5">
                  {EXAMPLES.map((example) => (
                    <button
                      key={example}
                      type="button"
                      disabled={send.busy}
                      onClick={() => void ask(example)}
                      className="flex h-auto min-h-[44px] w-full justify-start rounded-[6px] border border-hairline bg-canvas px-3 py-2 text-left whitespace-normal text-[13.5px] text-ink-2 transition-colors hover:border-action hover:text-ink md:min-h-0"
                    >
                      {example}
                    </button>
                  ))}
                </div>
              </div>
            )}
            {conversation.list.map((message) => (
              <Bubble key={message.id} person={message.role === "user"} content={message.content} />
            ))}
            {conversation.working && (
              <div className="flex flex-col items-start" role="status">
                <div className="rounded-[10px] rounded-bl-[3px] border border-hairline bg-canvas px-3.5 py-2 text-[13px] text-ink-2">
                  <span className="mr-2 inline-flex gap-1 align-middle" aria-hidden>
                    <span className="h-1.5 w-1.5 animate-bounce rounded-full bg-ai [animation-delay:-0.2s]" />
                    <span className="h-1.5 w-1.5 animate-bounce rounded-full bg-ai [animation-delay:-0.1s]" />
                    <span className="h-1.5 w-1.5 animate-bounce rounded-full bg-ai" />
                  </span>
                  <em>Working on it…</em> It keeps working if you leave; this page checks again every 2 seconds.
                </div>
              </div>
            )}
          </div>
          <div className="border-t border-hairline px-4 py-3">
            <AskComposer
              compact
              label={proposing ? "Change something" : "Message"}
              placeholder="An agent that flags renewals at risk every Monday"
              busy={send.busy}
              error={send.error}
              onSend={ask}
            />
          </div>
        </section>
        <TurnInStreams taskId={taskId} />
      </Split>
    </Page>
  );
}

/** One message: the person's on the right, Agent Creator Agent's (Markdown) on the left. */
function Bubble({ person, content }: { person: boolean; content: string }) {
  return (
    <div className={cx("flex flex-col", person ? "items-end" : "items-start")}>
      <div className="mb-1 font-mono text-[10.5px] tracking-[0.08em] text-ink-3 uppercase">{person ? "You" : "Agent Creator Agent"}</div>
      {person ? (
        <div className="max-w-[85%] rounded-[10px] rounded-br-[3px] bg-action-filled px-3.5 py-2 text-[13.5px] break-words whitespace-pre-wrap text-on-action">{content}</div>
      ) : (
        <div className="ask-reply max-w-full rounded-[10px] rounded-bl-[3px] border border-hairline bg-canvas px-3 py-2.5 text-[13.5px] break-words md:max-w-[92%] md:px-4">
          <Markdown>{content}</Markdown>
        </div>
      )}
    </div>
  );
}

/** Agent Creator Agent's proposal (the page's #proposal panel), with Approve and create. */
function Proposal({ answer, busy, error, onApprove }: { answer: Row; busy: boolean; error: unknown; onApprove: () => void }) {
  const proposal = (answer.proposal && typeof answer.proposal === "object" ? answer.proposal : {}) as Row;
  const agent = (proposal.agent && typeof proposal.agent === "object" ? proposal.agent : {}) as Row;
  const area = (agent.work_area && typeof agent.work_area === "object" ? agent.work_area : {}) as Row;
  const defects: unknown[] = Array.isArray(answer.defects) ? answer.defects : [];
  const files: Row[] = Array.isArray(proposal.files) ? proposal.files : [];
  const pipelines = (Array.isArray(proposal.pipelines) ? (proposal.pipelines as Row[]) : []).map((g) => `${text(g, "graph")} in ${text(g, "project")}`).join(", ");
  return (
    <div id="proposal">
      <Card
        label="Proposal"
        title={`Proposed Agent: ${text(agent, "name")}`}
        tone={defects.length ? "exception" : "active"}
        actions={<Chip tone="ai">{text(proposal, "type") || "agent"}</Chip>}
        footer={
          defects.length ? undefined : (
            <div className="flex flex-col gap-2">
              {error ? <ErrorBlock error={error} /> : null}
              <Button variant="primary" loading={busy} onClick={onApprove} className="w-full justify-center">
                <Check size={14} aria-hidden /> Approve and create
              </Button>
            </div>
          )
        }
      >
        <p className="mt-0 text-[13.5px] text-ink">{text(agent, "purpose")}</p>
        <KeyValue
          className="text-[13px]"
          rows={[
            ["Type", `${text(proposal, "type")} — ${text(proposal, "why_type")}`],
            ["Stored in", text(proposal, "type") === "python" ? `${text(proposal, "storage")} — ${text(proposal, "why_storage")}` : "the workspace (native)"],
            ["Work area", `${text(area, "kind")} ${text(area, "detail")}`.trim()],
            ["Instructions", text(agent, "manifesto")],
            ["What success looks like", text(agent, "goal")],
            ["Skills", skillLabels(agent.skills)],
            ["Trigger", text(agent, "trigger") || "Conversation only"],
            ["Pipelines", pipelines || "—"],
          ]}
        />
        {files.length > 0 && (
          <div className="mt-3 space-y-2 border-t border-hairline pt-3">
            <div className="label m-0">Files</div>
            {files.map((file, i) => (
              <details key={i} className="rounded-[5px] border border-hairline">
                <summary className="flex cursor-pointer items-center gap-1.5 px-3 py-1.5 font-mono text-[12px]">
                  <FileCode2 size={13} aria-hidden /> {text(file, "path")}
                </summary>
                <pre className="m-0 max-h-[40vh] overflow-auto rounded-none border-0 border-t border-hairline text-[12px]">
                  <code>{text(file, "contents")}</code>
                </pre>
              </details>
            ))}
          </div>
        )}
        {defects.length ? (
          <div className="mt-3">
            <Banner tone="warn" title="This proposal has defects and cannot be created yet:">
              <ul className="my-1 pl-5">
                {defects.map((defect, i) => (
                  <li key={i}>{typeof defect === "string" ? defect : JSON.stringify(defect)}</li>
                ))}
              </ul>
              Tell the Agent Creator Agent what to change.
            </Banner>
          </div>
        ) : null}
      </Card>
    </div>
  );
}
