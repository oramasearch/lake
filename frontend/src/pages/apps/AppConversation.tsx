/** Change an App in a conversation with the Application Agent (lake#1240,
 * docs/ui/apps.md#changing-an-app-in-a-conversation). The thread is `get_app_conversation` (each
 * person's own, kept in the workspace); a message is `change_app`; the agent answers, asks back or
 * proposes a diff; Confirm is the existing `agent_turn` confirmation (kind `change`), checked again
 * against the stored proposal before it is sent; and the thread goes on after. */
import { type FormEvent, useEffect, useRef, useState } from "react";
import { Bot, Loader2, Send, Wrench } from "lucide-react";
import { act, ApiError, useRead } from "@/lib/api";
import { Button, cx, ErrorBlock, Notice, Skeleton } from "@/lib/ui";
import { canonicalJson, fetchAgentTask, Markdown, PageError, taskResult, Terminal, useRunner } from "./shared";
import { confirmable, confirmInput, type Conversation, entries, newlyApplied, sendInput, threadWorking, type Turn, turnResult } from "./conversation";

export function AppConversation({
  appId,
  customerId,
  workspaceId,
  agentDeployed,
  onApplied,
  className,
}: {
  appId: string;
  customerId: string;
  workspaceId: string | null;
  agentDeployed: boolean;
  /** A confirmed change was committed and deployed (its confirmation turn succeeded). */
  onApplied?: (turn: Turn) => void;
  className?: string;
}) {
  const runner = useRunner();
  const [draft, setDraft] = useState("");
  const conversation = useRead<Conversation>(customerId && appId ? "get_app_conversation" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined, app_id: appId }, {
    refetchInterval: (query) => (threadWorking((query.state.data as Conversation | undefined)?.turns ?? []) ? 2000 : false),
  });
  const turns = conversation.data?.turns ?? [];
  const previous = useRef<Turn[] | undefined>(undefined);
  const bottom = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!conversation.data) return;
    for (const turn of newlyApplied(previous.current, conversation.data.turns)) onApplied?.(turn);
    previous.current = conversation.data.turns;
    bottom.current?.scrollIntoView?.({ block: "nearest" });
  }, [conversation.data, onApplied]);

  const send = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const message = draft.trim();
    if (!message) return;
    void runner
      .run("send", () => act("change_app", sendInput(appId, customerId, workspaceId, message)))
      .then((started) => {
        if (started === undefined) return;
        setDraft("");
        void conversation.refetch();
      });
  };

  const proposal = confirmable(turns);
  const confirm = async () => {
    if (!proposal) return;
    // Lake checks the same again: the stored proposal, unchanged since it was shown, of this app.
    const fresh = await fetchAgentTask(proposal.task_id, customerId, workspaceId);
    const stored = taskResult(fresh);
    if (fresh.status !== "succeeded" || stored.kind !== "proposal" || stored.confirm !== true) {
      throw new ApiError(400, "invalid_input", "That turn has no Application Agent proposal to confirm.");
    }
    if (canonicalJson(stored) !== canonicalJson(turnResult(proposal))) {
      throw new ApiError(409, "conflict", "The Application Agent proposal changed before confirmation.");
    }
    await act("agent_turn", confirmInput({ ...proposal, proposal_sha256: fresh.proposal_sha256 ?? proposal.proposal_sha256, conversation_id: fresh.conversation_id ?? proposal.conversation_id }, appId, customerId, workspaceId));
    await conversation.refetch();
  };

  const working = threadWorking(turns);
  return (
    <div className={cx("flex min-h-0 flex-col", className)} aria-label="Conversation with the Application Agent">
      <PageError error={runner.error} />
      <div className="min-h-0 flex-1 space-y-3 overflow-auto" aria-live="polite">
        {conversation.isLoading ? (
          <Skeleton lines={3} />
        ) : conversation.isError ? (
          <ErrorBlock error={conversation.error} retry={() => void conversation.refetch()} what="this app's conversation" />
        ) : turns.length === 0 ? (
          <p className="muted m-0 text-[13px]">
            Tell the Application Agent what to change ("add a chart by region"). It answers, asks back or proposes a diff; nothing changes until you confirm it. The conversation is kept with this app.
          </p>
        ) : (
          entries(turns).map((entry) =>
            entry.role === "you" ? (
              <div key={entry.id} className="ml-8 rounded-[8px] bg-surface-2 px-3 py-2 text-[13px]" data-role="you">
                {entry.confirmation ? <em>{entry.text}</em> : entry.text}
              </div>
            ) : (
              <div key={entry.id} className="mr-4 text-[13px]" data-role="agent" data-state={entry.state}>
                <div className="mb-1 inline-flex items-center gap-1.5 text-[12px] font-semibold text-ink-2">
                  <Bot size={13} aria-hidden /> Application Agent
                  {entry.state === "working" ? <Loader2 size={13} className="animate-spin" aria-hidden /> : null}
                </div>
                {entry.state === "failed" ? (
                  <Notice tone="danger">{entry.text}</Notice>
                ) : (
                  <div className="ask-reply">
                    <Markdown>{entry.text}</Markdown>
                  </div>
                )}
                {entry.state === "proposal" ? (
                  <>
                    <p className="muted mb-1 mt-2 text-[12.5px]">
                      {entry.files} file{entry.files === 1 ? "" : "s"} change.{entry.current ? " Confirmed, the agent commits them to the app's repository and redeploys." : entry.confirmed ? " You confirmed it; what happened is below." : " A later message replaced this proposal."}
                    </p>
                    <Terminal title="Diff" empty="No diff.">
                      {entry.diff ?? ""}
                    </Terminal>
                    {entry.current ? (
                      <div className="mt-2">
                        <Button variant="primary" loading={runner.busy === "confirm"} onClick={() => void runner.run("confirm", confirm)}>
                          <Wrench size={14} aria-hidden /> Confirm and apply the change
                        </Button>
                      </div>
                    ) : null}
                  </>
                ) : null}
                {entry.state === "applied" && entry.diff ? (
                  <details className="mt-1">
                    <summary className="cursor-pointer text-[12.5px] text-ink-2">What changed</summary>
                    <Terminal className="mt-2" title="Diff">
                      {entry.diff}
                    </Terminal>
                  </details>
                ) : null}
              </div>
            ),
          )
        )}
        {conversation.data?.truncated ? <p className="muted m-0 text-[12px]">Only the latest turns are shown.</p> : null}
        <div ref={bottom} />
      </div>
      <form className="stack mt-3" onSubmit={send}>
        <label>
          <span className="sr-only">Message to the Application Agent</span>
          <textarea
            name="message"
            rows={3}
            value={draft}
            onChange={(event) => setDraft(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) event.currentTarget.form?.requestSubmit();
            }}
            placeholder={turns.length ? "Now make the chart by region…" : "Add a chart by region, filter by plan…"}
            disabled={!agentDeployed}
            aria-label="Message to the Application Agent"
          />
        </label>
        <div className="flex flex-wrap items-center gap-2">
          <Button type="submit" variant="primary" disabled={!agentDeployed || !draft.trim() || working} loading={runner.busy === "send"} title={working ? "Wait for the agent to answer" : undefined}>
            <Send size={14} aria-hidden /> Send
          </Button>
          {working ? <span className="muted text-[12.5px]">The agent is working; it keeps going if you leave.</span> : null}
        </div>
      </form>
      {!agentDeployed && <Notice tone="warning">The Application Agent is not running on this Lake.</Notice>}
    </div>
  );
}
