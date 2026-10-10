/** Ask Orama, the full page (web_ui.cpp web_ui_ask). `conversation`, `task`, `agent` and `from`
 * stay in the URL as they did, so a conversation is linkable and the panel's "Full view" opens it
 * here. */
import { useCallback } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Page } from "@/lib/ui";
import { useTrail } from "@/lib/trail";
import { qs, useAsync, useWorkspace } from "@/pages/train/workspace";
import { AskComposer, AskMessages, TurnInStreams, sendMessage, useAppPath, useConversation, useTalkTo } from "./conversation";

export default function AskPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const path = useAppPath();
  const { customerId, workspaceId } = useWorkspace();
  const conversationId = params.get("conversation") ?? "";
  const taskId = params.get("task") ?? "";
  const from = params.get("from") ?? "";
  const agent = params.get("agent") || "agent-ask-orama";
  const spaceParam = params.get("customer_id") ?? "";
  useTrail([{ label: "Ask Orama" }]);

  const go = useCallback(
    (values: { conversation?: string; task?: string; agent?: string }, replace = true) => {
      navigate(
        path(
          `/ask?${qs({
            customer_id: spaceParam,
            conversation: values.conversation ?? conversationId,
            from,
            agent: values.agent ?? (params.get("agent") || ""),
            task: values.task,
          })}`,
        ),
        { replace },
      );
    },
    [navigate, path, spaceParam, conversationId, from, params],
  );
  const onSettled = useCallback(() => go({ task: "" }), [go]);
  const conversation = useConversation({ kind: "ask", conversationId, taskId, onSettled });
  const agents = useTalkTo(agent);
  const send = useAsync();

  return (
    <Page
      eyebrow="Ask Orama"
      title="Ask Orama"
      sub="Ask about anything in your lake. Ask Orama reads what you may read, proposes every change for you to confirm, and brings in the right agent when another does it better."
    >
      {from && (
        <p className="sub">
          About the page <Link to={path(from)}>{from.split("?")[0]}</Link>
        </p>
      )}
      <AskMessages
        conversationId={conversationId}
        list={conversation.list}
        loading={conversation.messages.isLoading}
        error={conversation.messages.error ?? conversation.settleError ?? conversation.task.error}
        working={conversation.working}
        task={conversation.task.data}
        agent={agent}
      />
      <TurnInStreams taskId={taskId} />
      <section className="panel">
        <AskComposer
          agent={agent}
          agents={agents}
          onAgent={(next) => go({ agent: next, task: taskId })}
          busy={send.busy}
          error={send.error}
          onSend={(message) =>
            send.run(async () => {
              const answer = await sendMessage({ customerId, workspaceId, conversationId, kind: "ask", agent, message, from });
              go({ conversation: answer.conversation_id, task: answer.task_id ?? "", agent }, false);
            })
          }
        />
        <p className="sub">
          <Link to={path(`/ask?${qs({ customer_id: spaceParam, from })}`)}>Start a new conversation</Link>
        </p>
      </section>
    </Page>
  );
}
