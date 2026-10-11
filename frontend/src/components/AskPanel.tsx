/** Ask Orama beside every page, replacing the server-rendered dock and its iframe: a header, the
 * conversation, and the message box at the bottom. `from` is the page it was opened from (path and
 * query), which every turn is told about. The conversation is the one the Ask Orama page shows
 * (pages/ask/conversation.tsx); "Full view" opens it there.
 *
 * The panel works in the session's current workspace. Its conversation is remembered for this tab
 * per workspace, so closing and reopening the panel, or moving between pages, keeps it.
 *
 * On a phone it is a full-screen conversation (docs/ui/mobile-web.md): sized to the visual
 * viewport so the message box sits just above the keyboard, the page behind locked, inside the safe
 * areas, following each new answer unless the person scrolled up to read. A page can open it about
 * something (`openAsk`, lib/ask.ts): the request's context goes with the next message. */
import { type CSSProperties, useCallback, useEffect, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { ExternalLink, FileText, Maximize2, Minimize2, Plus, Sparkles, X } from "lucide-react";
import { useSession } from "@/lib/session";
import { ASK_DEFAULT_WIDTH, setResizingAsk } from "@/lib/askWidth";
import type { AskRequest } from "@/lib/ask";
import { useIsPhone, usePhoneSheet } from "@/lib/phoneSheet";
import { NewReplyChip, useFollow } from "@/components/follow";
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
  expanded = false,
  onToggleExpanded,
  request,
  onRequestDone,
}: {
  onClose: () => void;
  from: string;
  width: number;
  onResize: (width: number) => void;
  overlay: boolean;
  /** Widened over the page (the expand button), until it is collapsed again. */
  expanded?: boolean;
  onToggleExpanded?: () => void;
  /** What a page opened the panel about, until it is sent or dismissed. */
  request?: AskRequest | null;
  onRequestDone?: () => void;
}) {
  const { workspaceId, space } = useSession();
  const phone = useIsPhone();
  const keyboard = usePhoneSheet(phone);
  // The home indicator's strip matters only while the keyboard is down.
  const style = { width, ...(phone ? { "--sheet-safe-bottom": keyboard ? "0px" : "env(safe-area-inset-bottom)" } : {}) } as CSSProperties;
  return (
    <aside className="ask-panel" data-overlay={overlay || undefined} data-expanded={expanded || undefined} data-sheet={phone || undefined} style={style} aria-label="Ask Orama">
      {!expanded && <ResizeHandle width={width} onResize={onResize} />}
      <div className="ask-head flex h-12 shrink-0 items-center gap-2 border-b border-[var(--lake-border-subtle)] px-4">
        <Sparkles size={15} aria-hidden />
        <div className="min-w-0 flex-1">
          <h2 className="m-0 text-base">Ask Orama</h2>
          <p className="muted m-0 truncate text-xs" title={from}>
            {from ? `About the page ${from.split("?")[0]}` : "Ask about anything in your lake. Changes are proposed for you to confirm."}
          </p>
        </div>
        {workspaceId && space ? <PanelActions workspaceId={workspaceId} from={from} /> : null}
        {!phone && onToggleExpanded && (
          <button type="button" className="icon-button" onClick={onToggleExpanded} title={expanded ? "Back to the side" : "Expand over the page"} aria-label={expanded ? "Back to the side" : "Expand over the page"} aria-pressed={expanded}>
            {expanded ? <Minimize2 size={15} aria-hidden /> : <Maximize2 size={15} aria-hidden />}
          </button>
        )}
        <button type="button" className="icon-button" onClick={onClose} aria-label="Close Ask Orama">
          <X size={16} aria-hidden />
        </button>
      </div>
      {workspaceId && space ? (
        <PanelConversation key={workspaceId} workspaceId={workspaceId} from={from} request={request ?? null} onRequestDone={onRequestDone} />
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
      <Link to={full} className="icon-button" title="Open as a page" aria-label="Open the conversation as a page">
        <ExternalLink size={15} aria-hidden />
      </Link>
    </>
  );
}

function PanelConversation({ workspaceId, from, request, onRequestDone }: { workspaceId: string; from: string; request: AskRequest | null; onRequestDone?: () => void }) {
  const [state, update] = usePanelState(workspaceId);
  const { space } = useSession();
  const onSettled = useCallback(() => update({ task: "" }), [update]);
  const conversation = useConversation({ kind: "ask", conversationId: state.conversation, taskId: state.task, onSettled });
  const agents = useTalkTo(state.agent);
  const send = useAsync();
  const scroller = useRef<HTMLDivElement>(null);
  const count = conversation.list.length;
  const working = conversation.working;
  const follow = useFollow(scroller, `${count}/${working}/${conversation.task.data?.message ?? ""}`, !count && !working);
  return (
    <>
      <div className="relative flex min-h-0 flex-1 flex-col">
        <div ref={scroller} onScroll={follow.onScroll} className="ask-scroll min-h-0 flex-1 overflow-x-hidden overflow-y-auto overscroll-contain px-4 py-3">
          <AskMessages
            conversationId={state.conversation}
            list={conversation.list}
            loading={conversation.messages.isLoading}
            error={conversation.messages.error ?? conversation.settleError ?? conversation.task.error}
            working={working}
            task={conversation.task.data}
            agent={state.agent}
          />
          <TurnInStreams taskId={state.task} />
        </div>
        <NewReplyChip show={follow.chip} onClick={() => follow.toLatest()} />
      </div>
      <div className="ask-foot border-t border-[var(--lake-border-subtle)] px-4 py-3">
        <AskComposer
          compact
          agent={state.agent}
          agents={agents}
          onAgent={(agent) => update({ agent })}
          busy={send.busy}
          error={send.error}
          draft={request?.draft}
          context={
            request ? (
              <div className="ask-context flex min-w-0 items-center gap-2 rounded-[6px] border border-hairline bg-raised px-2.5 py-1.5 text-[12.5px] text-ink-2">
                <FileText size={13} className="shrink-0" aria-hidden />
                <span className="min-w-0 flex-1 truncate" title={request.label}>
                  {request.label}
                </span>
                <button type="button" className="plain hit shrink-0 rounded text-ink-3 hover:text-ink" onClick={onRequestDone} aria-label="Ask without this context">
                  <X size={13} aria-hidden />
                </button>
              </div>
            ) : null
          }
          onSend={(message) =>
            send.run(async () => {
              follow.markAsked();
              const answer = await sendMessage({
                customerId: space?.id ?? "",
                workspaceId,
                conversationId: state.conversation,
                kind: "ask",
                agent: state.agent,
                message,
                from,
                page: request?.page,
              });
              update({ conversation: answer.conversation_id, task: answer.task_id ?? "" });
              // The context went with this message; the next one is about the page again.
              if (request) onRequestDone?.();
            })
          }
        />
      </div>
    </>
  );
}
