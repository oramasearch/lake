/** Ask Orama beside every page, replacing the server-rendered dock and its iframe: a header, the
 * conversation, and the message box at the bottom. `from` is the page it was opened from (path and
 * query), which every turn is told about. The conversation is the one the Ask Orama page shows
 * (pages/ask/conversation.tsx); "Full view" opens it there.
 *
 * The panel works in the session's current workspace. Its conversation is remembered for this tab
 * per workspace, so closing and reopening the panel, or moving between pages, keeps it. */
import { useCallback, useEffect, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { Maximize2, Plus, Sparkles, X } from "lucide-react";
import { useSession } from "@/lib/session";
import { ASK_DEFAULT_WIDTH, setResizingAsk } from "@/lib/askWidth";
import { qs, useAsync } from "@/pages/train/workspace";
import { AskComposer, AskMessages, TurnInStreams, sendMessage, useAppPath, useConversation, useTalkTo } from "@/pages/ask/conversation";

type PanelState = { conversation: string; task: string; agent: string };
const EMPTY: PanelState = { conversation: "", task: "", agent: "agent-ask-orama" };
const KEY = "lake-ask-panel:";

function recall(workspaceId: string): PanelState {
  try {
    const stored = JSON.parse(sessionStorage.getItem(KEY + workspaceId) ?? "null");
    return stored && typeof stored === "object" ? { ...EMPTY, ...stored } : EMPTY;
  } catch {
    return EMPTY;
  }
}

function keep(workspaceId: string, state: PanelState) {
  try {
    sessionStorage.setItem(KEY + workspaceId, JSON.stringify(state));
  } catch {
    /* storage blocked: the conversation lasts while the panel is open */
  }
}

/** The panel's left edge: drag it, use the arrow keys on it, or double-click it to reset. */
function ResizeHandle({ width, onResize }: { width: number; onResize: (width: number) => void }) {
  const [active, setActive] = useState(false);
  const start = (event: React.PointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    const startX = event.clientX;
    const startWidth = width;
    setResizingAsk(true);
    setActive(true);
    document.body.style.cursor = "col-resize";
    document.body.style.userSelect = "none";
    const move = (e: PointerEvent) => onResize(startWidth + (startX - e.clientX));
    const up = () => {
      setResizingAsk(false);
      setActive(false);
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };
  return (
    <div
      role="separator"
      aria-orientation="vertical"
      aria-label="Resize Ask Orama"
      aria-valuenow={width}
      tabIndex={0}
      title="Drag to resize · double-click to reset"
      onPointerDown={start}
      onDoubleClick={() => onResize(ASK_DEFAULT_WIDTH)}
      onKeyDown={(e) => {
        if (e.key === "ArrowLeft") onResize(width + 24);
        else if (e.key === "ArrowRight") onResize(width - 24);
        else return;
        e.preventDefault();
      }}
      className="group absolute top-0 bottom-0 -left-[3px] z-10 hidden w-[7px] cursor-col-resize focus:outline-none md:block"
    >
      <div className={active ? "mx-auto h-full w-[2px] bg-action" : "mx-auto h-full w-[2px] bg-transparent group-hover:bg-action/60 group-focus-visible:bg-action"} />
    </div>
  );
}

export function AskPanel({
  onClose,
  from,
  width,
  onResize,
  overlay,
}: {
  onClose: () => void;
  from: string;
  width: number;
  onResize: (width: number) => void;
  overlay: boolean;
}) {
  const { workspaceId, space } = useSession();
  return (
    <aside className="ask-panel" data-overlay={overlay || undefined} style={{ width }} aria-label="Ask Orama">
      <ResizeHandle width={width} onResize={onResize} />
      <div className="flex items-center gap-2 border-b border-[var(--lake-border-subtle)] px-4 py-2">
        <Sparkles size={15} aria-hidden />
        <div className="min-w-0 flex-1">
          <h2 className="m-0 text-base">Ask Orama</h2>
          <p className="muted m-0 truncate text-xs" title={from}>
            {from ? `About the page ${from.split("?")[0]}` : "Ask about anything in your lake. Changes are proposed for you to confirm."}
          </p>
        </div>
        {workspaceId && space ? <PanelActions workspaceId={workspaceId} from={from} /> : null}
        <button type="button" className="icon-button" onClick={onClose} aria-label="Close Ask Orama">
          <X size={16} aria-hidden />
        </button>
      </div>
      {workspaceId && space ? (
        <PanelConversation key={workspaceId} workspaceId={workspaceId} from={from} />
      ) : (
        <div className="p-4">
          <p>Ask Orama works in a workspace, and you have none selected.</p>
          <p>
            <Link to="/workspaces">Choose or start one on Workspaces</Link>.
          </p>
        </div>
      )}
    </aside>
  );
}

/** Every panel control reading the same workspace's remembered state hears a change. */
const listeners = new Set<() => void>();
function announce() {
  listeners.forEach((l) => l());
}

function usePanelState(workspaceId: string): [PanelState, (next: Partial<PanelState>) => void] {
  const [state, setState] = useState<PanelState>(() => recall(workspaceId));
  useEffect(() => {
    const listener = () => setState(recall(workspaceId));
    listeners.add(listener);
    return () => {
      listeners.delete(listener);
    };
  }, [workspaceId]);
  const update = useCallback(
    (next: Partial<PanelState>) => {
      const merged = { ...recall(workspaceId), ...next };
      keep(workspaceId, merged);
      announce();
    },
    [workspaceId],
  );
  return [state, update];
}

function PanelActions({ workspaceId, from }: { workspaceId: string; from: string }) {
  const [state, update] = usePanelState(workspaceId);
  const path = useAppPath();
  const full = path(`/ask?${qs({ conversation: state.conversation, from, agent: state.agent === "agent-ask-orama" ? "" : state.agent, task: state.task })}`);
  return (
    <>
      <button type="button" className="icon-button" onClick={() => update({ ...EMPTY, agent: state.agent })} title="Start a new conversation" aria-label="Start a new conversation">
        <Plus size={16} aria-hidden />
      </button>
      <Link to={full} className="icon-button" title="Full view" aria-label="Open in the Ask Orama page">
        <Maximize2 size={15} aria-hidden />
      </Link>
    </>
  );
}

function PanelConversation({ workspaceId, from }: { workspaceId: string; from: string }) {
  const [state, update] = usePanelState(workspaceId);
  const { space } = useSession();
  const onSettled = useCallback(() => update({ task: "" }), [update]);
  const conversation = useConversation({ kind: "ask", conversationId: state.conversation, taskId: state.task, onSettled });
  const agents = useTalkTo(state.agent);
  const send = useAsync();
  const scroller = useRef<HTMLDivElement>(null);
  const count = conversation.list.length;
  useEffect(() => {
    scroller.current?.scrollTo({ top: scroller.current.scrollHeight });
  }, [count, conversation.working]);
  return (
    <>
      <div ref={scroller} className="min-h-0 flex-1 overflow-y-auto overscroll-contain px-4 py-3">
        <AskMessages
          conversationId={state.conversation}
          list={conversation.list}
          loading={conversation.messages.isLoading}
          error={conversation.messages.error ?? conversation.settleError ?? conversation.task.error}
          working={conversation.working}
          task={conversation.task.data}
          agent={state.agent}
        />
        <TurnInStreams taskId={state.task} />
      </div>
      <div className="border-t border-[var(--lake-border-subtle)] px-4 py-3">
        <AskComposer
          compact
          agent={state.agent}
          agents={agents}
          onAgent={(agent) => update({ agent })}
          busy={send.busy}
          error={send.error}
          onSend={(message) =>
            send.run(async () => {
              const answer = await sendMessage({
                customerId: space?.id ?? "",
                workspaceId,
                conversationId: state.conversation,
                kind: "ask",
                agent: state.agent,
                message,
                from,
              });
              update({ conversation: answer.conversation_id, task: answer.task_id ?? "" });
            })
          }
        />
      </div>
    </>
  );
}
