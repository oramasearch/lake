/** Ask Orama, the home of a signed-in person (`/`, lake#600): the conversation the side panel holds
 * (pages/ask/thread.ts), across the page's width in one centred, readable column, its message box at
 * the foot. Empty, it welcomes the person with starting questions drawn from the workspace's
 * datasets, projects and runs (`starterQuestions`, ./home.ts).
 *
 * A link's query hands the conversation over and is then dropped (`linkThread`): `conversation`,
 * `task` and `agent` (old `/ask` links, "Talk to this agent in Ask Orama"); `from` stays, the page
 * the panel's "Open as a page" came from. On this page the top bar's Ask Orama button opens no panel:
 * it puts the cursor in the message box (`focusAsk`, lib/ask.ts). */
import { useCallback, useEffect, useMemo, useRef } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import { Plus, Sparkles, X } from "lucide-react";
import { onFocusAsk } from "@/lib/ask";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Button } from "@/lib/ui";
import { NewReplyChip, useFollow } from "@/components/follow";
import { datasetChoices, type J } from "@/pages/design/logic";
import { useAsync, useWq } from "@/pages/train/workspace";
import { AskComposer, AskMessages, TurnInStreams, sendMessage, useConversation, useTalkTo } from "./conversation";
import { greeting, linkThread, starterQuestions } from "./home";
import { EMPTY_THREAD, useThread } from "./thread";

export default function AskHome() {
  useTrail([{ label: "Home", to: "/" }, { label: "Ask Orama" }], "Ask Orama");
  const { workspaceId, space, viewer } = useSession();
  if (!workspaceId || !space) {
    return (
      <div className="ask-home">
        <div className="ask-home-scroll">
          <div>
            <Welcome name={viewer.display_name} />
            <p>Ask Orama works in a workspace, and you have none selected.</p>
            <p>
              <Link to="/workspaces">Choose or start one on Workspaces</Link>.
            </p>
          </div>
        </div>
      </div>
    );
  }
  return <HomeConversation key={workspaceId} workspaceId={workspaceId} customerId={space.id} />;
}

function Welcome({ name }: { name?: string }) {
  return (
    <header className="ask-home-welcome">
      <p className="eyebrow">
        <Sparkles size={13} aria-hidden /> Ask Orama
      </p>
      <h1>{greeting(name)}</h1>
      <p className="sub">
        Ask about anything in your workspace: its data, projects, runs, models, services and agents. Ask Orama reads what you may read, proposes every change for you to
        confirm, and brings in the right agent when another does it better.
      </p>
    </header>
  );
}

function HomeConversation({ workspaceId, customerId }: { workspaceId: string; customerId: string }) {
  const { viewer, workspace } = useSession();
  const location = useLocation();
  const navigate = useNavigate();
  const [state, update] = useThread(workspaceId);

  // A link's conversation, task or agent goes into the shared conversation; only `from` stays.
  useEffect(() => {
    const { adopt, keep } = linkThread(location.search);
    if (adopt) update(adopt);
    if (location.search !== keep) navigate(`/${keep}`, { replace: true });
  }, [location.search, navigate, update]);
  const from = new URLSearchParams(location.search).get("from") ?? "";

  const onSettled = useCallback(() => update({ task: "" }), [update]);
  const conversation = useConversation({ kind: "ask", conversationId: state.conversation, taskId: state.task, onSettled });
  const agents = useTalkTo(state.agent);
  const send = useAsync();
  const scroller = useRef<HTMLDivElement>(null);
  const foot = useRef<HTMLDivElement>(null);
  const count = conversation.list.length;
  const working = conversation.working;
  const empty = !state.conversation && !working;
  const follow = useFollow(scroller, `${count}/${working}/${conversation.task.data?.message ?? ""}`, !count && !working);

  // The top bar's Ask Orama button, on this page: the cursor goes to the message box.
  useEffect(() => {
    return onFocusAsk(() => foot.current?.querySelector("textarea")?.focus());
  }, []);

  const ask = (message: string) =>
    send.run(async () => {
      follow.markAsked();
      const answer = await sendMessage({ customerId, workspaceId, conversationId: state.conversation, kind: "ask", agent: state.agent, message, from });
      update({ conversation: answer.conversation_id, task: answer.task_id ?? "" });
    });

  return (
    <div className="ask-home">
      <div className="relative flex min-h-0 flex-1 flex-col">
        <div ref={scroller} onScroll={follow.onScroll} className="ask-home-scroll">
          <div>
            {empty ? (
              <>
                <Welcome name={viewer.display_name} />
                <Starters workspaceName={workspace?.name ?? ""} busy={send.busy} onAsk={(q) => void ask(q)} />
              </>
            ) : (
              <>
                <div className="ask-home-bar">
                  <p className="eyebrow m-0">
                    <Sparkles size={13} aria-hidden /> Ask Orama
                  </p>
                  <Button variant="ghost" onClick={() => update({ ...EMPTY_THREAD, agent: state.agent })}>
                    <Plus size={14} aria-hidden /> New conversation
                  </Button>
                </div>
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
              </>
            )}
          </div>
        </div>
        <NewReplyChip show={follow.chip} onClick={() => follow.toLatest()} />
      </div>
      <div ref={foot} className="ask-home-foot">
        <div>
          {from && (
            <p className="sub ask-home-from">
              <span className="min-w-0 truncate" title={from}>
                About the page <Link to={from}>{from.split("?")[0]}</Link>
              </span>
              <button type="button" className="plain hit rounded text-ink-3 hover:text-ink" onClick={() => navigate("/", { replace: true })} aria-label="Ask without this page">
                <X size={13} aria-hidden />
              </button>
            </p>
          )}
          <AskComposer agent={state.agent} agents={agents} onAgent={(agent) => update({ agent })} busy={send.busy} error={send.error} onSend={ask} />
        </div>
      </div>
    </div>
  );
}

/** Starting questions from what the workspace holds; each asks it at once. */
function Starters({ workspaceName, busy, onAsk }: { workspaceName: string; busy: boolean; onAsk: (question: string) => void }) {
  const datasets = useWq<J>("list_datasets", {}, { staleTime: 60_000 });
  const projects = useWq<J>("list_projects", {}, { staleTime: 60_000 });
  const runs = useWq<J>("list_runs", {}, { staleTime: 60_000 });
  const settled = !datasets.isLoading && !projects.isLoading && !runs.isLoading;
  const questions = useMemo(
    () =>
      starterQuestions({
        datasets: datasetChoices(datasets.data?.datasets).map((d) => d.name),
        projects: Array.isArray(projects.data?.projects) ? projects.data!.projects.filter((p: unknown) => typeof p === "string") : [],
        runs: Array.isArray(runs.data?.runs) ? runs.data!.runs : [],
      }),
    [datasets.data, projects.data, runs.data],
  );
  return (
    <section className="ask-starters" aria-label="Starting questions">
      <p className="eyebrow">{workspaceName ? `Start with ${workspaceName}` : "Start here"}</p>
      {settled ? (
        <div className="ask-starter-grid">
          {questions.map((question) => (
            <button key={question} type="button" className="ask-starter" disabled={busy} onClick={() => onAsk(question)}>
              {question}
            </button>
          ))}
        </div>
      ) : (
        <div className="ask-starter-grid" aria-busy="true">
          {[0, 1, 2, 3].map((i) => (
            <div key={i} className="ask-starter ask-starter-loading" />
          ))}
        </div>
      )}
    </section>
  );
}
