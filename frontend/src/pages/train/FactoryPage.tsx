/** Train → Agents → Agent Factory (web_ui.cpp web_ui_factory, docs/api/agent-factory.md): a
 * conversation with AgentCreator, its proposal, and Approve and create. Nothing is created until the
 * person approves; Approve sends the SHA-256 of the proposal they saw, which the server checks
 * against the proposal stored in the conversation. */
import { useCallback, useMemo } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { act, type Row } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { AskComposer, MessageArticle, TurnInStreams, sendMessage, useConversation } from "@/pages/ask/conversation";
import { proposalDigest } from "@/pages/ask/digest";
import { skillLabels } from "./agents";
import { qs, text, useAsync, useWorkspace, useWsPath } from "./workspace";

export default function FactoryPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const ws = useWsPath();
  const toast = useToast();
  const { customerId, workspaceId } = useWorkspace();
  const conversationId = params.get("conversation") ?? "";
  const taskId = params.get("task") ?? "";
  const query = qs({ customer_id: customerId });
  useTrail([{ label: "Train" }, { label: "Agents", to: ws(`/train/agents?${query}`) }, { label: "Agent Factory" }]);

  const go = useCallback(
    (conversation: string, task: string, replace = true) => {
      navigate(ws(`/train/agents/new?${qs({ customer_id: params.get("customer_id"), conversation, task })}`), { replace });
    },
    [navigate, ws, params],
  );
  const onSettled = useCallback(() => go(conversationId, ""), [go, conversationId]);
  const conversation = useConversation({ kind: "factory", conversationId, taskId, onSettled });
  const send = useAsync();
  const approve = useAsync();

  // AgentCreator's latest structured answer: the last agent message that carries one.
  const latest = useMemo(() => {
    let found: { answer: Row; messageId: string } | null = null;
    for (const message of conversation.list) {
      const answer = message.payload && typeof message.payload === "object" ? message.payload.answer : undefined;
      if (message.role === "assistant" && answer !== undefined) found = { answer: answer && typeof answer === "object" ? answer : {}, messageId: message.id };
    }
    return found;
  }, [conversation.list]);
  const proposing = latest?.answer.kind === "proposal";

  return (
    <Page
      eyebrow="Train"
      title="Agent Factory"
      sub="Tell AgentCreator what you want an agent for. It asks what it must, then proposes the agent. Nothing is created until you approve the proposal."
    >
      <section className="panel ask-conversation" aria-live="polite">
        {conversation.messages.isError ? <ErrorBlock error={conversation.messages.error} what="this conversation" /> : null}
        {conversation.settleError ? <ErrorBlock error={conversation.settleError} /> : null}
        {conversation.messages.isLoading && <Skeleton lines={2} />}
        {conversation.list.map((message) => (
          <MessageArticle key={message.id} person={message.role === "user"} who={message.role === "user" ? "You" : "AgentCreator"} content={message.content} />
        ))}
        {!conversation.messages.isLoading && !conversation.list.length && <p>Describe the agent: what it is for, what it may touch, and what done looks like.</p>}
        {conversation.working && (
          <article className="ask-message ask-agent" role="status">
            <p className="eyebrow">AgentCreator</p>
            <p>
              <em>Working on it…</em> It keeps working if you leave; this page checks again every 2 seconds.
            </p>
          </article>
        )}
      </section>
      <TurnInStreams taskId={taskId} />
      {proposing && latest && (
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
                proposal_sha256: proposalDigest(latest.answer.proposal ?? null),
              });
              let words = `Created ${created.name ?? "the agent"}.`;
              if (created.schedule_error) words += ` Its trigger was not scheduled: ${created.schedule_error}`;
              if (created.check_launch_id) words += " A first check run has started.";
              toast(words, created.schedule_error ? "bad" : "ok");
              navigate(created.agent_id ? ws(`/train/agent?${qs({ customer_id: customerId, id: created.agent_id })}`) : ws(`/train/agents?${query}`));
            })
          }
        />
      )}
      <section className="panel">
        <AskComposer
          label={proposing ? "Change something" : "Message"}
          placeholder="An agent that flags renewals at risk every Monday"
          busy={send.busy}
          error={send.error}
          onSend={(message) =>
            send.run(async () => {
              const answer = await sendMessage({ customerId, workspaceId, conversationId, kind: "factory", agent: "agent-creator", message, from: "" });
              go(answer.conversation_id, answer.task_id ?? "", false);
            })
          }
        />
        <p className="sub">
          <Link to={ws(`/train/agents?${query}`)}>Back to agents</Link>
        </p>
      </section>
    </Page>
  );
}

/** AgentCreator's proposal (the page's #proposal panel). */
function Proposal({ answer, busy, error, onApprove }: { answer: Row; busy: boolean; error: unknown; onApprove: () => void }) {
  const proposal = (answer.proposal && typeof answer.proposal === "object" ? answer.proposal : {}) as Row;
  const agent = (proposal.agent && typeof proposal.agent === "object" ? proposal.agent : {}) as Row;
  const area = (agent.work_area && typeof agent.work_area === "object" ? agent.work_area : {}) as Row;
  const defects: unknown[] = Array.isArray(answer.defects) ? answer.defects : [];
  const files: Row[] = Array.isArray(proposal.files) ? proposal.files : [];
  const pipelines = (Array.isArray(proposal.pipelines) ? (proposal.pipelines as Row[]) : []).map((g) => `${text(g, "graph")} in ${text(g, "project")}`).join(", ");
  return (
    <section className="panel" id="proposal">
      <h2>Proposed Agent: {text(agent, "name")}</h2>
      <p>{text(agent, "purpose")}</p>
      <dl>
        <dt>Type</dt>
        <dd>
          {text(proposal, "type")} — {text(proposal, "why_type")}
        </dd>
        <dt>Stored in</dt>
        <dd>{text(proposal, "type") === "python" ? `${text(proposal, "storage")} — ${text(proposal, "why_storage")}` : "the workspace (native)"}</dd>
        <dt>Work area</dt>
        <dd>
          {text(area, "kind")} {text(area, "detail")}
        </dd>
        <dt>Instructions</dt>
        <dd>{text(agent, "manifesto")}</dd>
        <dt>What success looks like</dt>
        <dd>{text(agent, "goal")}</dd>
        <dt>What it may do</dt>
        <dd>{skillLabels(agent.skills)}</dd>
        <dt>Trigger</dt>
        <dd>{text(agent, "trigger") || "Conversation only"}</dd>
        <dt>Pipelines</dt>
        <dd>{pipelines || "—"}</dd>
      </dl>
      {files.map((file, i) => (
        <div key={i}>
          <h4>
            <code>{text(file, "path")}</code>
          </h4>
          <pre>
            <code>{text(file, "contents")}</code>
          </pre>
        </div>
      ))}
      {defects.length ? (
        <div className="error" role="alert">
          <p>This proposal has defects and cannot be created yet:</p>
          <ul>
            {defects.map((defect, i) => (
              <li key={i}>{typeof defect === "string" ? defect : JSON.stringify(defect)}</li>
            ))}
          </ul>
          <p>Tell AgentCreator what to change.</p>
        </div>
      ) : (
        <div className="stack">
          {error ? <ErrorBlock error={error} /> : null}
          <div>
            <Button variant="primary" loading={busy} onClick={onApprove}>
              Approve and create
            </Button>
          </div>
        </div>
      )}
    </section>
  );
}
