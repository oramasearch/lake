/** Ask Orama, the home of a signed-in person (`/`, lake#600): the conversation the side panel holds
 * (pages/ask/thread.ts), across the page's width in one centred, readable column, its message box at
 * the foot. Empty, it welcomes the person and explains what they can do (lake#764): starting
 * questions drawn from the workspace's datasets, projects and runs (`starterQuestions`, ./home.ts),
 * the agents they can talk to with what each does (the Agent Library, as Talk to lists them), and
 * what Lake does, grouped as the rail is, each page with a question to ask about it (`lakeMap`,
 * ./lakeMap.ts). A question goes into the message box to edit or send; picking an agent starts a
 * conversation with it. With no workspace the first step is explicit: Start a workspace.
 *
 * A link's query hands the conversation over and is then dropped (`linkThread`): `conversation`,
 * `task` and `agent` (old `/ask` links, "Talk to this agent in Ask Orama"); `from` stays, the page
 * the panel's "Open as a page" came from. On this page the top bar's Ask Orama button opens no panel:
 * it puts the cursor in the message box (`focusAsk`, lib/ask.ts). Delete removes the conversation
 * from the workspace after a typed confirmation (`delete_ask_conversation`, docs/ui/deletion.md). */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import { AppWindow, Bot, Crown, FlaskConical, FolderKanban, Lightbulb, type LucideIcon, Plus, Rocket, Sparkles, Trash2, WandSparkles, X } from "lucide-react";
import { SECTION_ICONS } from "@/lib/sectionIcons";
import { useRead } from "@/lib/api";
import { onFocusAsk } from "@/lib/ask";
import { conversationLines, DELETE_WORD } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Banner, Button } from "@/lib/ui";
import { navFor } from "@/routes";
import { useStartWorkspace } from "@/components/WorkspaceGate";
import type { TalkToAgent } from "@/lib/agents";
import { NewReplyChip, useFollow } from "@/components/follow";
import { datasetChoices, type J } from "@/pages/design/logic";
import { useAsync, useWq } from "@/pages/train/workspace";
import { AskComposer, AskMessages, TurnInStreams, sendMessage, useAppPath, useConversation, useTalkTo } from "./conversation";
import { greeting, linkThread, starterQuestions } from "./home";
import { lakeMap } from "./lakeMap";
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
            <FirstStep customerId={space?.id ?? viewer.customer_id ?? ""} hasWorkspaces={(space?.workspaces ?? []).length > 0} />
            <AgentsToTalkTo />
            <WhatLakeDoes />
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
      {/* The positioning (Steve, 2026-10-17): AI sovereignty, said without telling anyone what to do. */}
      <p className="ask-home-badge">
        <Crown size={14} aria-hidden /> AI sovereignty
      </p>
      <p className="ask-home-lead">When you're tired of renting someone else's AI, Orama Lake is where you own your AI.</p>
      <p className="sub">
        Your data and everything built from it stay in a workspace of your own. Ask Orama answers questions about anything in it, reads only what you may read,
        proposes every change for you to confirm, and brings in the right agent when another does it better.
      </p>
    </header>
  );
}

/** With no workspace, the first step (lake#764): start one, in one click, and why. A person who
 * has workspaces but none selected chooses one. Start is the workspace gate's own action. */
function FirstStep({ customerId, hasWorkspaces }: { customerId: string; hasWorkspaces: boolean }) {
  const { start, busy, error } = useStartWorkspace(customerId);
  return (
    <section className="ask-first-step" aria-labelledby="ask-first-step">
      <h2 id="ask-first-step">{hasWorkspaces ? "First step: choose a workspace" : "First step: start a workspace"}</h2>
      <p className="sub">
        Everything in Lake happens in a workspace: your data, projects, models and agents live there, apart from every other account's. Ask Orama answers about the
        workspace you are in.
      </p>
      {error && <Banner tone="warn">{error}</Banner>}
      <div className="flex flex-wrap items-center gap-2">
        {hasWorkspaces ? (
          <Button variant="primary" to="/workspaces">
            Choose a workspace
          </Button>
        ) : (
          <Button variant="primary" loading={busy} disabled={!customerId} onClick={() => void start()}>
            <Rocket size={14} aria-hidden /> Start a workspace
          </Button>
        )}
      </div>
      {!hasWorkspaces && <p className="muted m-0 text-[12.5px]">It takes a minute or two to prepare. You can read on meanwhile.</p>}
    </section>
  );
}

/** Each library agent's icon on the home (lake#786); a person's own agents use Bot. */
const AGENT_ICONS: Record<string, LucideIcon> = {
  "agent-ask-orama": Sparkles,
  "agent-project": FolderKanban,
  "agent-application": AppWindow,
  "agent-experiments": FlaskConical,
  "agent-autosuggest": Lightbulb,
  "agent-creator": WandSparkles,
};

function AgentIcon({ id }: { id: string }) {
  const Icon = AGENT_ICONS[id] ?? Bot;
  return (
    <span className="ask-icon-tile ask-icon-tile-sm" aria-hidden>
      <Icon size={15} />
    </span>
  );
}

/** The agents a person can talk to, with what each does (the Agent Library, as Talk to lists them,
 * lake#763). With `onPick`, choosing one starts a conversation with it. */
function AgentsToTalkTo({ selected = "", onPick }: { selected?: string; onPick?: (agent: string) => void }) {
  const agents = useTalkTo(selected).filter((a: TalkToAgent) => a.running && a.purpose);
  if (!agents.length) return null;
  return (
    <section className="ask-home-section" aria-labelledby="ask-agents">
      <h2 id="ask-agents">Agents you can talk to</h2>
      <div className="ask-map-grid">
        {agents.map((agent) =>
          onPick ? (
            <button key={agent.id} type="button" className="ask-starter ask-agent" aria-pressed={agent.id === selected} onClick={() => onPick(agent.id)}>
              <span className="ask-agent-name">
                <AgentIcon id={agent.id} /> {agent.name}
              </span>
              <span className="ask-agent-purpose">{agent.purpose}</span>
            </button>
          ) : (
            <div key={agent.id} className="ask-starter ask-agent">
              <span className="ask-agent-name">
                <AgentIcon id={agent.id} /> {agent.name}
              </span>
              <span className="ask-agent-purpose">{agent.purpose}</span>
            </div>
          ),
        )}
      </div>
    </section>
  );
}

function GroupIcon({ section }: { section: string }) {
  const Icon = SECTION_ICONS[section] ?? Sparkles;
  return (
    <span className="ask-icon-tile" aria-hidden>
      <Icon size={18} />
    </span>
  );
}

/** What you can do with Lake (lake#764): the rail's groups, only what this Lake runs, each page with
 * one question. With `onAsk`, a question goes into the message box. */
function WhatLakeDoes({ onAsk }: { onAsk?: (question: string) => void }) {
  const session = useSession();
  const path = useAppPath();
  const info = useRead<{ deployments?: boolean }>("server_info", undefined, { workspace: null, staleTime: Infinity });
  const library = useRead<{ agents?: { key?: string; deployed?: boolean }[] }>("list_agent_library", undefined, { staleTime: 300_000 });
  const groups = useMemo(() => {
    const running = new Set((library.data?.agents ?? []).filter((a) => a.deployed).map((a) => String(a.key)));
    return lakeMap(navFor(session), { deployments: info.data?.deployments, runningAgents: running });
  }, [session, info.data, library.data]);
  return (
    <section className="ask-home-section" aria-labelledby="ask-lake-map">
      <h2 id="ask-lake-map">Design, train and ship production-grade AI</h2>
      <p className="sub ask-home-section-line">Self-serve, real AI, trained on your own data.</p>
      <div className="ask-map">
        {groups.map((group) => (
          <section key={group.key} className="ask-map-group" aria-label={group.title}>
            <header className="ask-map-head">
              <GroupIcon section={group.key} />
              <h3>
                {group.title} <span className="muted">{group.line}</span>
              </h3>
            </header>
            <ul>
              {group.things.map((thing) => (
                <li key={thing.path}>
                  <Link to={path(thing.path)} className="ask-map-link">
                    {thing.label}
                  </Link>
                  <span className="ask-map-does">{thing.does}</span>
                  {onAsk && (
                    <button type="button" className="plain ask-map-ask" onClick={() => onAsk(thing.ask)} title="Put this question in the message box">
                      <Sparkles size={12} aria-hidden /> {thing.ask}
                    </button>
                  )}
                </li>
              ))}
            </ul>
          </section>
        ))}
      </div>
    </section>
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
  // A question the home puts in the message box (lake#764), and a count so the same one twice works.
  const [draft, setDraft] = useState({ text: "", key: 0 });
  const putInBox = useCallback((text: string) => setDraft((d) => ({ text, key: d.key + 1 })), []);
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
                <Starters workspaceName={workspace?.name ?? ""} onPick={putInBox} />
                <AgentsToTalkTo
                  selected={state.agent}
                  onPick={(agent) => {
                    update({ ...EMPTY_THREAD, agent });
                    foot.current?.querySelector("textarea")?.focus();
                  }}
                />
                <WhatLakeDoes onAsk={putInBox} />
              </>
            ) : (
              <>
                <div className="ask-home-bar">
                  <p className="eyebrow m-0">
                    <Sparkles size={13} aria-hidden /> Ask Orama
                  </p>
                  <span className="inline-flex flex-wrap items-center gap-1.5">
                    {state.conversation && !working ? (
                      <DeletionButton
                        variant="ghost"
                        capability="delete_ask_conversation"
                        input={{ customer_id: customerId, workspace_id: workspaceId, conversation_id: state.conversation }}
                        title="Delete This Conversation"
                        confirmLabel="Delete conversation"
                        intro="Its messages, and the agent tasks it started, are removed from this workspace. It cannot be undone."
                        typed={() => DELETE_WORD}
                        lines={conversationLines}
                        done="Conversation deleted"
                        onDone={() => update({ ...EMPTY_THREAD, agent: state.agent })}
                      >
                        <Trash2 size={14} aria-hidden /> Delete
                      </DeletionButton>
                    ) : null}
                    <Button variant="ghost" onClick={() => update({ ...EMPTY_THREAD, agent: state.agent })}>
                      <Plus size={14} aria-hidden /> New conversation
                    </Button>
                  </span>
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
          <AskComposer
            agent={state.agent}
            agents={agents}
            onAgent={(agent) => update({ agent })}
            busy={send.busy}
            error={send.error}
            onSend={ask}
            draft={draft.text}
            draftKey={draft.key}
          />
        </div>
      </div>
    </div>
  );
}

/** Suggested first questions from what the workspace holds; each goes into the message box to edit
 * or send (lake#764). */
function Starters({ workspaceName, onPick }: { workspaceName: string; onPick: (question: string) => void }) {
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
            <button key={question} type="button" className="ask-starter" onClick={() => onPick(question)} title="Put this question in the message box">
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
