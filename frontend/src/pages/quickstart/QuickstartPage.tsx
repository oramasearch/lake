/** Quickstart (`/` for a signed-in person, lake#1128, docs/ui/quickstart.md): drop a dataset and
 * Lake takes it to search and model answers on your own website. Every step is a capability
 * (start_quickstart, quickstart_status, train_quickstart, quickstart_snippets), so an agent can do
 * the same over MCP; the page shows each step's state, resumes where the person stopped, says what
 * a paid step costs before it runs, and offers the top-up link when the balance blocks it. */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Link } from "react-router-dom";
import { Brain, Code2, Database, KeyRound, MessageSquare, Plus, RotateCcw, Search, SearchX, Wallet } from "lucide-react";
import { act, ApiError, usePostRead, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Banner, Button, Card, CopyBlock, Empty, ErrorBlock, FilePicker, Page, Skeleton, StateTabs, Status } from "@/lib/ui";
import { addFundsPath } from "@/pages/billing/balance";
import { money } from "@/pages/billing/billing";
import { nameForFile } from "@/pages/train/upload";
import { AskComposer, AskMessages, sendMessage, useConversation } from "@/pages/ask/conversation";
import {
  MAX_UPLOAD_BYTES,
  blockedBy,
  columnUses,
  detected,
  hitLine,
  pollInterval,
  predictable,
  publishesNow,
  startingChoice,
  trainInput,
  trainLabel,
  valuesOf,
  type Choice,
  type WebsiteView,
  DEFAULT_WEBSITE_VIEW,
  WEBSITE_VIEWS,
  agentPrompt,
  type Column,
  rateLabel,
  stepTone,
  uploadContent,
  withKey,
  keyCovers,
  modelStepTitle,
  modelView,
  showsTraining,
  isClustering,
  elapsed,
  failureReason,
  fixMessage,
  opensConversation,
  whereItLives,
  siteOrigin,
  type Costs,
  type Snippets,
  type Status as QuickstartStatus,
} from "./quickstart";

/** What the page holds while a new workspace starts: the file, so the same start goes on once it is
 * ready without asking for it again. */
type Pending = { content: string; format: "csv" | "jsonl"; name: string };

export default function QuickstartPage() {
  useTrail([{ label: "Quickstart" }], "drop a dataset, get search and model answers on your website");
  const session = useSession();
  const { viewer } = session;
  const customerId = session.space?.id ?? viewer.customer_id ?? "";
  const [workspaceId, setWorkspaceId] = useState<string>(session.workspaceId ?? "");
  const [quickstartId, setQuickstartId] = useState<string>("");
  const [fresh, setFresh] = useState(false);
  const status = useRead<QuickstartStatus>(
    "quickstart_status",
    { customer_id: customerId, workspace_id: workspaceId, ...(quickstartId ? { quickstart_id: quickstartId } : {}) },
    {
      enabled: !!customerId && !!workspaceId,
      refetchInterval: (query) => pollInterval((query.state.data as QuickstartStatus | undefined)?.state),
    },
  );
  const costs = useRead<QuickstartStatus>(
    "quickstart_status",
    { customer_id: customerId, workspace_id: workspaceId, include_costs: true },
    { enabled: !!customerId && !!workspaceId, staleTime: 5 * 60_000 },
  );
  const current = fresh ? null : status.data && status.data.quickstart_id ? status.data : null;
  const balance = viewer.billing ? viewer.balance ?? costs.data?.costs?.balance ?? null : null;
  const blocked = blockedBy(balance);

  return (
    <Page
      eyebrow="Quickstart"
      title="Search and Model Answers on Your Website"
      sub="Drop a dataset. Lake adds it to your workspace, builds a search index you can try here and put on your website, then trains a model on the same data so your site shows its answers too."
      actions={
        current ? (
          <Button
            data-testid="quickstart-start-another"
            onClick={() => {
              setFresh(true);
              setQuickstartId("");
            }}
          >
            <Plus size={14} aria-hidden /> Start another
          </Button>
        ) : null
      }
    >
      <div className="grid gap-4" data-testid="quickstart-page">
        {!customerId ? (
          <Banner tone="warn">Lake could not tell which account to use. Choose a workspace first.</Banner>
        ) : workspaceId && status.isLoading ? (
          <Skeleton lines={4} />
        ) : workspaceId && status.isError ? (
          <ErrorBlock error={status.error} retry={() => void status.refetch()} what="the quickstart" />
        ) : (
          <>
            <Steps status={fresh ? null : status.data ?? null} />
            <Others status={status.data ?? null} chosen={quickstartId} onChoose={(id) => { setFresh(false); setQuickstartId(id); }} />
            {!current ? (
              <DropStep
                customerId={customerId}
                workspaceId={workspaceId}
                waiting={status.data?.state === "waiting_for_workspace" ? status.data : null}
                workspaceReady={status.data?.workspace?.ready === true && status.data.state === "not_started"}
                costs={costs.data?.costs}
                blocked={blocked}
                onStarted={(answer) => {
                  if (answer.workspace_id) setWorkspaceId(answer.workspace_id);
                  if (answer.quickstart_id) {
                    setFresh(false);
                    setQuickstartId(answer.quickstart_id);
                  }
                  void status.refetch();
                }}
              />
            ) : (
              <Started status={current} costs={costs.data?.costs} blocked={blocked} onChanged={() => void status.refetch()} />
            )}
          </>
        )}
      </div>
    </Page>
  );
}

/** Each step with its state: done, running, failed and why, or still to come. */
function Steps({ status }: { status: QuickstartStatus | null }) {
  const steps = status?.steps ?? [
    { step: "workspace", title: "Workspace", state: "pending" as const, error: null },
    { step: "dataset", title: "Dataset", state: "pending" as const, error: null },
    { step: "search_index", title: "Search Index", state: "pending" as const, error: null },
    { step: "website", title: "Search on Your Website", state: "pending" as const, error: null },
    { step: "model", title: "Model", state: "pending" as const, error: null },
    { step: "model_answers", title: "Model Answers on Your Website", state: "pending" as const, error: null },
  ];
  return (
    <Card label="The quickstart process" title="Steps">
      <ol className="m-0 grid list-none gap-1.5 p-0" aria-label="Quickstart steps" data-testid="quickstart-steps">
        {steps.map((step, i) => (
          <li key={step.step} className="grid grid-cols-[1.5rem_minmax(0,1fr)_auto] items-start gap-2" data-testid={`quickstart-step-${step.step}`} data-state={step.state}>
            <span className="font-mono text-[12px] text-ink-3 tabular-nums">{i + 1}</span>
            <span className="min-w-0">
              <span className="text-[13.5px] text-ink">{step.title}</span>
              {step.error ? <span className="block text-[12.5px] text-bad">{step.error.message}</span> : null}
              {step.note ? <span className="block text-[12.5px] text-ink-3">{step.note}</span> : null}
            </span>
            <Status value={step.state} tone={stepTone(step.state)} />
          </li>
        ))}
      </ol>
    </Card>
  );
}

/** The workspace's other quickstarts, to open one again. */
function Others({ status, chosen, onChoose }: { status: QuickstartStatus | null; chosen: string; onChoose: (id: string) => void }) {
  const list = status?.quickstarts ?? [];
  if (list.length < 2) return null;
  return (
    <label className="block max-w-[28rem]">
      <span className="label m-0 block">Your quickstarts</span>
      <select className="w-full" value={chosen || status?.quickstart_id || ""} onChange={(e) => onChoose(e.target.value)} data-testid="quickstart-choose">
        {list.map((q) => (
          <option key={q.quickstart_id} value={q.quickstart_id}>
            {q.dataset_id ?? "no dataset yet"} · {q.created_at ?? ""}
          </option>
        ))}
      </select>
    </label>
  );
}

/** What a paid step is billed by, at the published rates, with the balance. */
function CostNote({ costs, step }: { costs: Costs | undefined; step: string }) {
  const entry = costs?.steps.find((s) => s.step === step);
  if (!costs || !entry) return null;
  const currency = costs.currency || "USD";
  const rated = entry.billed_by.map((d) => [d.title, rateLabel(d.rate, d.unit, (m) => money(m, currency))] as const).filter(([, label]) => label);
  return (
    <div className="text-[12.5px] text-ink-2" data-testid={`quickstart-cost-${step}`}>
      <p className="m-0">
        What it costs: {entry.note}.{" "}
        {costs.billing ? "It is charged against your prepaid balance." : "This Lake does not bill."}
      </p>
      {rated.length ? (
        <ul className="mt-1 mb-0 pl-4">
          {rated.map(([title, label]) => (
            <li key={title}>
              {title}: {label}
            </li>
          ))}
        </ul>
      ) : null}
      {costs.balance ? <p className="mt-1 mb-0">Your balance: {money(costs.balance.balance_micros, currency)}.</p> : null}
    </div>
  );
}

/** The balance blocks paid work: the top-up link, never a step that fails. */
function TopUp({ customerId }: { customerId: string }) {
  return (
    <Banner
      tone="warn"
      title="Add funds to continue"
      action={
        <Button variant="primary" to={addFundsPath(customerId)} data-testid="quickstart-top-up">
          <Wallet size={14} aria-hidden /> Add funds
        </Button>
      }
    >
      Your prepaid balance does not cover new work yet. Add funds, then come back: the quickstart goes on from here.
    </Banner>
  );
}

function isBlocked(error: unknown): boolean {
  return error instanceof ApiError && error.code === "payment_required";
}

/** Step 1: drop a dataset. Without a workspace one is made, and the start goes on once it is ready. */
function DropStep({
  customerId,
  workspaceId,
  waiting,
  workspaceReady,
  costs,
  blocked,
  onStarted,
}: {
  customerId: string;
  workspaceId: string;
  waiting: QuickstartStatus | null;
  workspaceReady: boolean;
  costs: Costs | undefined;
  blocked: boolean;
  onStarted: (answer: QuickstartStatus) => void;
}) {
  const [file, setFile] = useState<File | null>(null);
  const [problem, setProblem] = useState<string>("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const [pending, setPending] = useState<Pending | null>(null);
  const resumed = useRef(false);

  const start = async (upload: Pending, workspace: string) => {
    setBusy(true);
    setError(null);
    try {
      const answer = await act<QuickstartStatus>("start_quickstart", {
        customer_id: customerId,
        ...(workspace ? { workspace_id: workspace } : {}),
        content: upload.content,
        format: upload.format,
        name: upload.name,
      });
      if (answer.state === "waiting_for_workspace") {
        resumed.current = false;
        setPending(upload);
      } else setPending(null);
      onStarted(answer);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  // The workspace this page asked for became ready: the same start goes on, once.
  useEffect(() => {
    if (pending && workspaceReady && workspaceId && !resumed.current) {
      resumed.current = true;
      void start(pending, workspaceId);
    }
  }, [pending, workspaceReady, workspaceId]);

  const choose = async (picked: File | null) => {
    setFile(picked);
    setProblem("");
    setError(null);
    if (!picked) return;
    if (picked.size > MAX_UPLOAD_BYTES) setProblem("The file is larger than 16 MiB. Load larger data from Data → Datasets or a connection.");
  };

  const send = async () => {
    if (!file) return;
    const read = uploadContent(file.name, await file.text());
    if ("error" in read) {
      setProblem(read.error);
      return;
    }
    resumed.current = false;
    await start({ ...read, name: nameForFile("", "", file.name) || "quickstart" }, workspaceId);
  };

  return (
    <Card label="Step 1" title="Drop a Dataset" tone="active">
      <p className="mt-0 text-[13px] text-ink-2">
        A CSV, JSON or JSON Lines file of up to 16 MiB. Lake adds it to your workspace{workspaceId ? "" : ", making one for you,"} and builds a search index over every column.
      </p>
      <div data-testid="quickstart-drop">
        <FilePicker file={file} onFile={(f) => void choose(f)} accept=".csv,.json,.jsonl,.ndjson,.parquet" name="dataset" prompt="Drop your dataset here or choose a file" hint="CSV, JSON or JSON Lines" disabled={busy || !!pending} />
      </div>
      {problem ? <p className="text-[12.5px] text-bad" role="alert">{problem}</p> : null}
      {pending || waiting ? (
        <Banner tone="info" title="Making your workspace">
          Your workspace is starting{waiting?.workspace?.phase ? ` (${waiting.workspace.phase})` : ""}. {pending ? "Your dataset goes in as soon as it is ready; keep this page open." : "You can add your dataset now: it goes in as soon as the workspace is ready."}
        </Banner>
      ) : null}
      {isBlocked(error) || blocked ? (
        <TopUp customerId={customerId} />
      ) : (
        <>
          {error ? <ErrorBlock error={error} /> : null}
          <CostNote costs={costs} step="search_index" />
          <div className="mt-3">
            <Button variant="primary" loading={busy} disabled={!file || !!problem || !!pending} onClick={() => void send()} data-testid="quickstart-start">
              <Database size={14} aria-hidden /> Add it and build search
            </Button>
          </div>
        </>
      )}
    </Card>
  );
}

/** Steps 2 to 5, once there is a quickstart: the live search box, the website code, training and
 * the model's answers. */
function Started({ status, costs, blocked, onChanged }: { status: QuickstartStatus; costs: Costs | undefined; blocked: boolean; onChanged: () => void }) {
  const searchable = !!status.index?.published;
  const stopped = status.state === "failed" && status.steps.some((s) => s.state === "failed" && (s.step === "dataset" || s.step === "search_index"));
  return (
    <>
      {stopped ? <Resume status={status} onChanged={onChanged} /> : null}
      {/* lake#1204: a dataset whose search is not built yet (the page was left part way) goes on
          from here, never a page with only the step list. */}
      {!stopped && !searchable ? <Resume status={status} onChanged={onChanged} building /> : null}
      {searchable ? <TrySearch status={status} /> : null}
      {searchable ? <Website status={status} /> : null}
      {searchable ? <ModelStep status={status} costs={costs} blocked={blocked} onChanged={onChanged} /> : null}
    </>
  );
}

/** A dataset or search step failed: why, and try it again from where it stopped. `building`:
 * nothing failed, but the search index is not built yet; the same call builds it. */
function Resume({ status, onChanged, building }: { status: QuickstartStatus; onChanged: () => void; building?: boolean }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const failed = status.steps.find((s) => s.state === "failed" && (s.step === "dataset" || s.step === "search_index"));
  return (
    <Card label={building ? "Step 2" : "Try again"} title={building ? "Build the Search Index" : "Resume the Quickstart"} tone={building ? "active" : "exception"}>
      <p className="mt-0 text-[13px] text-ink-2" data-testid="quickstart-resume-why">
        {building
          ? `${status.dataset?.name ?? "Your dataset"} is in your workspace; its search index is not built yet. Build it to search your data here and on your website.`
          : failed?.error?.message ?? "A step stopped before it finished. Try it again from where it stopped."}
      </p>
      {isBlocked(error) ? <TopUp customerId={status.customer_id} /> : error ? <ErrorBlock error={error} /> : null}
      <Button
        variant="primary"
        loading={busy}
        data-testid="quickstart-resume"
        onClick={() => {
          setBusy(true);
          setError(null);
          act("start_quickstart", { customer_id: status.customer_id, workspace_id: status.workspace_id, quickstart_id: status.quickstart_id })
            .then(onChanged, setError)
            .finally(() => setBusy(false));
        }}
      >
        {building ? "Build the search index" : "Try the failed step again"}
      </Button>
    </Card>
  );
}

/** Step 2: a search box over the person's own data, with the model's answer beside each result
 * once it is published, exactly as the website will show them. */
function TrySearch({ status }: { status: QuickstartStatus }) {
  const [typed, setTyped] = useState("");
  const [query, setQuery] = useState("");
  useEffect(() => {
    const timer = setTimeout(() => setQuery(typed.trim()), 250);
    return () => {
      clearTimeout(timer);
    };
  }, [typed]);
  const found = useRead<{ hits?: Row[] }>(
    "search_index",
    { customer_id: status.customer_id, workspace_id: status.workspace_id, index_id: status.index?.id ?? "", query, top_k: 10 },
    { enabled: !!query && !!status.index?.id },
  );
  const hits = useMemo(() => found.data?.hits ?? [], [found.data]);
  const modelId = status.model?.published ? status.model.id : "";
  const rows = useMemo(() => hits.map((hit) => ({ ...(hit.fields ?? {}) })), [hits]);
  // predict changes nothing; it is POSTed because rows go in a body.
  const answers = usePostRead<{ ranked?: { row: number; risk: number }[] }>(
    "predict",
    { customer_id: status.customer_id, workspace_id: status.workspace_id, model_id: modelId, rows },
    { enabled: !!modelId && rows.length > 0 },
  );
  const answerAt = new Map<number, number>((answers.data?.ranked ?? []).map((entry) => [entry.row, entry.risk]));
  const target = status.training?.target ?? "model";
  return (
    <Card label="Step 2" title="Search Your Data" tone="active">
      <form className="flex flex-wrap items-end gap-2" role="search" onSubmit={(e) => e.preventDefault()}>
        <label className="min-w-[14rem] flex-1">
          <span className="label m-0 block">Search {status.dataset?.name ?? "your dataset"}</span>
          <input type="search" className="w-full" value={typed} onChange={(e) => setTyped(e.target.value)} placeholder="Type anything in your data" data-testid="quickstart-search" />
        </label>
      </form>
      <div className="mt-3" data-testid="quickstart-hits">
        {!query ? (
          <Empty compact icon={Search} title="Try your search">
            Results appear as you type, ranked, with each row's values{modelId ? " and the model's answer" : ""}.
          </Empty>
        ) : found.isLoading ? (
          <Skeleton />
        ) : found.isError ? (
          <ErrorBlock error={found.error} retry={() => void found.refetch()} what="the search" />
        ) : hits.length === 0 ? (
          <Empty compact icon={SearchX} title="Nothing matched">
            Try other words.
          </Empty>
        ) : (
          <ol className="m-0 list-none divide-y divide-hairline rounded-[6px] border border-hairline p-0" aria-label="Search results">
            {hits.map((hit, i) => (
              <li key={`${String(hit.id)}-${i}`} className="flex flex-wrap items-baseline justify-between gap-2 px-3 py-2 text-[13px]" data-testid="quickstart-hit">
                <span className="min-w-0 break-words text-ink">{hitLine(hit.fields as Record<string, unknown> | undefined)}</span>
                {answerAt.has(i) ? (
                  <span className="font-mono text-[12px] text-ai" data-testid="quickstart-answer">
                    {target}: {answerAt.get(i)!.toFixed(2)}
                  </span>
                ) : null}
              </li>
            ))}
          </ol>
        )}
        {answers.isError ? <ErrorBlock error={answers.error} what="the model's answers" /> : null}
      </div>
    </Card>
  );
}

type Lang = "html" | "react" | "curl";

/** Steps 3 and 5: the code for a website, with Lake's host and the ids filled in, and a browser key
 * made here in one click for the website's address and put into it (lake#1152). */
function Website({ status }: { status: QuickstartStatus }) {
  const where = {
    customer_id: status.customer_id,
    workspace_id: status.workspace_id,
    ...(status.quickstart_id ? { quickstart_id: status.quickstart_id } : {}),
  };
  // Without `origins` quickstart_snippets changes nothing: it is POSTed, as a read.
  const snippets = usePostRead<Snippets>("quickstart_snippets", where);
  const [view, setView] = useState<WebsiteView>(DEFAULT_WEBSITE_VIEW);
  const [lang, setLang] = useState<Lang>("html");
  const [site, setSite] = useState("");
  const [made, setMade] = useState<Snippets | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const value = snippets.data;
  const typed = site ? siteOrigin(site) : null;
  // A key made before the model was published can only search: the code needs a new one.
  const stale = !!made?.key && !!value?.key && !keyCovers(made.key.capabilities, value.key.capabilities);
  const shown = made && !stale ? made : value;
  const snippet = shown?.snippets.find((s) => s.id === lang);
  const secret = made && !stale ? made.key?.secret ?? null : null;
  const prompt = agentPrompt(shown, secret);
  return (
    <Card label={status.model?.published ? "Steps 3 and 5" : "Step 3"} title={status.model?.published ? "Search and Model Answers on Your Website" : "Search on Your Website"}>
      {snippets.isLoading ? (
        <Skeleton />
      ) : snippets.isError ? (
        <ErrorBlock error={snippets.error} retry={() => void snippets.refetch()} what="the website code" />
      ) : value && value.ready ? (
        <div data-testid="quickstart-website">
          <ol className="mt-0 pl-4 text-[13px] text-ink-2">
            <li>Type your website's address and make its key. The key only searches this index{value.includes_model ? " and asks your model" : ""}, only from that address, and it is shown once.</li>
            <li>Copy the agent prompt into your coding agent, or the code into your page yourself: a plain web page or a React component.</li>
            <li>Open the page and search. {value.includes_model ? "Each result shows the model's answer beside it." : "Train a model below to add its answers."}</li>
          </ol>
          {error ? <ErrorBlock error={error} /> : null}
          {stale ? <Banner tone="info">Your key can only search. Make a new key that can also ask your model, and copy the code again.</Banner> : null}
          {secret ? (
            <CopyBlock value={secret} label={`Your website key for ${made?.key?.browser_key?.origins.join(", ") ?? "your website"}`} />
          ) : (
            <form
              className="flex flex-wrap items-end gap-2"
              onSubmit={(e) => {
                e.preventDefault();
                if (!typed || "error" in typed) return;
                setBusy(true);
                setError(null);
                act<Snippets>("quickstart_snippets", { ...where, origins: [typed.origin] })
                  .then(setMade, setError)
                  .finally(() => setBusy(false));
              }}
            >
              <label className="grid min-w-[16rem] flex-1 gap-1 text-[12px] text-ink-2">
                Your website's address
                <input type="url" value={site} onChange={(e) => setSite(e.target.value)} placeholder="https://www.example.com" data-testid="quickstart-site" aria-invalid={!!typed && "error" in typed} />
              </label>
              <Button type="submit" loading={busy} disabled={!typed || "error" in typed} data-testid="quickstart-create-key">
                <KeyRound size={14} aria-hidden /> Make a website key
              </Button>
              {typed && "error" in typed ? <p className="m-0 basis-full text-[12px] text-bad">{typed.error}</p> : null}
            </form>
          )}
          <div className="mt-3" data-testid="quickstart-website-views">
            <StateTabs<WebsiteView> value={prompt ? view : "code"} onChange={setView} label="How to add it" tabs={prompt ? WEBSITE_VIEWS : WEBSITE_VIEWS.filter((v) => v.value === "code")} />
          </div>
          {prompt && view === "prompt" ? (
            <div data-testid="quickstart-agent-prompt">
              <p className="mb-1 mt-2 text-[12.5px] text-ink-2">
                Paste this into your coding agent (Claude Code, Cursor and the like) in your website's project. It holds the code, {secret ? "your key, " : ""}what the key can do, what Lake answers and how to check it works.
              </p>
              <CopyBlock value={prompt} label="Agent prompt" multiline />
            </div>
          ) : (
            <>
              <div className="mt-3">
                <StateTabs<Lang>
                  value={lang}
                  onChange={setLang}
                  label="Code"
                  tabs={[
                    { value: "html", label: "Plain web page" },
                    { value: "react", label: "React" },
                    { value: "curl", label: "curl" },
                  ]}
                />
              </div>
              {snippet ? (
                <div data-testid={`quickstart-snippet-${snippet.id}`}>
                  <CopyBlock value={withKey(snippet.code, value.key?.placeholder ?? "YOUR_LAKE_SEARCH_KEY", secret)} multiline />
                </div>
              ) : null}
            </>
          )}
          {shown?.key ? (
            <p className="mb-0 text-[12px] text-ink-3">
              {shown.key.note} <Link to={`/w/${encodeURIComponent(status.workspace_id)}/ship/index?${new URLSearchParams({ customer_id: status.customer_id, id: status.index?.id ?? "" })}`}>Website keys</Link>
            </p>
          ) : null}
          <p className="mb-0 text-[12px] text-ink-3">
            <Code2 size={12} aria-hidden className="inline" /> An AI agent reaches the same index through the MCP tool <code>search_index</code> at <code>{value.origin}/mcp</code>.
          </p>
        </div>
      ) : (
        <Banner tone="quiet">{value?.next ?? "The search index is not published yet."}</Banner>
      )}
    </Card>
  );
}

/** Step 4 (lake#1182, docs/ui/quickstart.md#step-4-decide-what-to-build): an Ask Orama conversation
 * about the dataset just added, opened on its own once search works. The agent proposes what is
 * worth building (a model, a derived target, clustering, opportunities, an app such as an analysis
 * dashboard, an analysis, a generated corpus) and the person confirms in the chat. Confirming a
 * model runs train_quickstart, and the step becomes the training below. "Choose the columns myself"
 * stays as a small fallback. Never a dead end. */
function ModelStep({ status, costs, blocked, onChanged }: { status: QuickstartStatus; costs: Costs | undefined; blocked: boolean; onChanged: () => void }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const opened = useRef("");
  const open = useCallback(
    (restart = false) => {
      setBusy(true);
      setError(null);
      act<QuickstartStatus>("start_quickstart_conversation", {
        customer_id: status.customer_id,
        workspace_id: status.workspace_id,
        quickstart_id: status.quickstart_id,
        ...(restart ? { restart: true } : {}),
      })
        .then(onChanged, setError)
        .finally(() => setBusy(false));
    },
    [status.customer_id, status.workspace_id, status.quickstart_id, onChanged],
  );
  // Opened once per quickstart, as soon as search works: the person never has to ask for it.
  useEffect(() => {
    if (opensConversation(status, blocked) && opened.current !== status.quickstart_id) {
      opened.current = status.quickstart_id ?? "";
      open();
    }
  }, [status, blocked, open]);
  // lake#1204: once a model is asked for (Confirm in the conversation, or the columns), the step
  // becomes the training, and the page brings it into view.
  const training = showsTraining(status);
  const [talking, setTalking] = useState(false);
  const shown = useRef(training);
  useEffect(() => {
    if (training && !shown.current) document.querySelector("[data-testid='quickstart-train-card']")?.scrollIntoView?.({ block: "start", behavior: "smooth" });
    shown.current = training;
  }, [training]);
  if (training) {
    return (
      <>
        <Train status={status} costs={costs} blocked={blocked} onChanged={onChanged} onAsk={() => setTalking(true)} />
        {status.conversation ? (
          <details className="text-[12.5px]" data-testid="quickstart-conversation-done" open={talking} onToggle={(e) => setTalking((e.currentTarget as HTMLDetailsElement).open)}>
            <summary className="cursor-pointer text-ink-2">The conversation about {status.dataset?.name ?? "this dataset"}</summary>
            <QuickstartConversation status={status} onChanged={onChanged} />
          </details>
        ) : null}
      </>
    );
  }
  return (
    <Card
      label="Step 4"
      title={modelStepTitle(status)}
      tone="active"
      actions={
        status.conversation ? (
          <Button variant="ghost" loading={busy} onClick={() => open(true)} data-testid="quickstart-conversation-restart">
            <RotateCcw size={14} aria-hidden /> Start over
          </Button>
        ) : null
      }
    >
      <div data-testid="quickstart-model-step">
        <p className="mt-0 text-[13px] text-ink-2">
          Ask Orama has read {status.dataset?.name ?? "your dataset"} and proposes what is worth building on it: a model, opportunities, an app such as a dashboard, an analysis, or generated data first. Talk it through, then confirm what you want; nothing runs before you confirm.
        </p>
        {isBlocked(error) || (blocked && !status.conversation) ? (
          <TopUp customerId={status.customer_id} />
        ) : status.conversation ? (
          <QuickstartConversation status={status} onChanged={onChanged} />
        ) : error ? (
          <>
            <ErrorBlock error={error} />
            <Button variant="primary" loading={busy} onClick={() => open()} data-testid="quickstart-conversation-open">
              <MessageSquare size={14} aria-hidden /> Ask Orama what to build
            </Button>
          </>
        ) : (
          <div data-testid="quickstart-conversation-opening">
            <p className="text-[12.5px] text-ink-3">Opening Ask Orama on {status.dataset?.name ?? "your dataset"}…</p>
            <Skeleton lines={2} />
          </div>
        )}
        <details className="mt-3 text-[12.5px]" data-testid="quickstart-choose-columns">
          <summary className="cursor-pointer text-ink-2">Choose the columns myself</summary>
          <Train status={status} costs={costs} blocked={blocked} onChanged={onChanged} bare />
        </details>
      </div>
    </Card>
  );
}

/** The quickstart's Ask Orama conversation (lake#1182): the shared conversation, held in the
 * quickstart's workspace, with every message carrying the dataset's context (`conversation.page`). */
function QuickstartConversation({ status, onChanged }: { status: QuickstartStatus; onChanged: () => void }) {
  const opened = status.conversation!;
  const scope = useMemo(() => ({ customerId: status.customer_id, workspaceId: status.workspace_id }), [status.customer_id, status.workspace_id]);
  const [task, setTask] = useState(opened.task_id ?? "");
  useEffect(() => {
    setTask(opened.task_id ?? "");
  }, [opened.conversation_id, opened.task_id]);
  const onSettled = useCallback(() => setTask(""), []);
  const conversation = useConversation({ kind: "ask", conversationId: opened.conversation_id, taskId: task, onSettled, scope });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const send = async (message: string) => {
    setBusy(true);
    setError(null);
    try {
      const answer = await sendMessage({ ...scope, conversationId: opened.conversation_id, kind: "ask", agent: "agent-ask-orama", message, from: "", page: opened.page });
      setTask(answer.task_id ?? "");
      return true;
    } catch (e) {
      setError(e);
      return false;
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="grid gap-2" data-testid="quickstart-conversation" data-conversation={opened.conversation_id}>
      <AskMessages
        conversationId={opened.conversation_id}
        list={conversation.list}
        loading={conversation.messages.isLoading}
        error={conversation.messages.error ?? conversation.settleError ?? conversation.task.error}
        working={conversation.working}
        task={conversation.task.data}
        agent="agent-ask-orama"
        scope={scope}
        onConfirmed={() => onChanged()}
        empty={<p>Ask Orama is reading {status.dataset?.name ?? "your dataset"}.</p>}
        after={(capability) => {
          const where = whereItLives(capability);
          return where ? <Link to={`/w/${encodeURIComponent(status.workspace_id)}${where.path}`}>Open {where.label}</Link> : null;
        }}
      />
      <AskComposer busy={busy} error={error} onSend={send} compact label="Reply to Ask Orama" placeholder="Which idea is the most interesting? Can it predict something?" />
    </div>
  );
}

/** Step 4 once a model is asked for (lake#1181, lake#1204): its training, followed live, then
 * published by the page, so the website code grows its answers. Every state says what is happening
 * and gives one next action: training (how long so far, and what comes next); failed (why in plain
 * words, Try again, and Ask Orama to fix it); done (what to do now; for clustering, where the
 * segments are). With `bare` it is the "Choose the columns myself" form under the conversation:
 * the choices list only columns the model can predict, with their detected types, and the page
 * shows what Lake does with every column and the snapshot date its dates are read as of. */
function Train({ status, costs, blocked, onChanged, onAsk, bare }: { status: QuickstartStatus; costs: Costs | undefined; blocked: boolean; onChanged: () => void; onAsk?: () => void; bare?: boolean }) {
  const columns = status.dataset?.columns ?? [];
  const known = detected(columns);
  const names = columns.length ? columns.map((c) => c.name) : status.index?.fields ?? [];
  const targets = known ? predictable(columns) : names.map((name) => ({ name, label: name }));
  const [choice, setChoice] = useState<Choice>(() => startingChoice(status));
  const set = (change: Partial<Choice>) => setChoice((before) => ({ ...before, ...change }));
  const [busy, setBusy] = useState(false);
  const [asking, setAsking] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const model = status.steps.find((s) => s.step === "model");
  const values = known ? valuesOf(columns, choice.target) : [];
  const train = (input: Row) => {
    setBusy(true);
    setError(null);
    act("train_quickstart", { customer_id: status.customer_id, workspace_id: status.workspace_id, quickstart_id: status.quickstart_id, ...input })
      .then(onChanged, setError)
      .finally(() => setBusy(false));
  };
  // Trained: publish it now, as part of the click that trained it.
  const publishing = useRef("");
  useEffect(() => {
    if (publishesNow(status) && publishing.current !== status.quickstart_id && !blocked) {
      publishing.current = status.quickstart_id ?? "";
      train({});
    }
  }, [status.state, status.quickstart_id, blocked]);
  // "Ask Orama to fix it": what failed goes into the conversation, which proposes the corrected
  // change as a card to confirm (opened first when the person chose the columns themselves).
  const askToFix = async () => {
    setAsking(true);
    setError(null);
    try {
      const message = fixMessage(status);
      if (status.conversation) {
        await sendMessage({ customerId: status.customer_id, workspaceId: status.workspace_id, conversationId: status.conversation.conversation_id, kind: "ask", agent: "agent-ask-orama", message, from: "", page: status.conversation.page });
      } else {
        await act("start_quickstart_conversation", { customer_id: status.customer_id, workspace_id: status.workspace_id, quickstart_id: status.quickstart_id, message });
      }
      onAsk?.();
      onChanged();
    } catch (e) {
      setError(e);
    } finally {
      setAsking(false);
    }
  };

  const view = bare ? "choose" : modelView(status);
  const clustering = isClustering(status.training?.methodology);
  const preparation = status.training?.preparation ?? null;
  const derived = status.training?.derived_target;
  const asked = derived ? derived.rule : status.training?.target_value ? `${status.training.target} is ${status.training.target_value}` : status.training?.target;
  const dataset = status.dataset?.name ?? "your dataset";
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (view !== "training") return;
    const timer = setInterval(() => setNow(Date.now()), 15_000);
    return () => clearInterval(timer);
  }, [view]);
  const since = elapsed(status.training?.started_at ?? model?.progress?.started_at, now);
  const segmentsPath = `/w/${encodeURIComponent(status.workspace_id)}/train/model?${new URLSearchParams({ customer_id: status.customer_id, id: status.model?.id ?? "" })}`;

  const form = (
    <>
      <p className="mt-0 text-[13px] text-ink-2">Lake learns to answer a yes-or-no question about one column from the others, then answers it for every search result.</p>
      <div className="grid gap-2 sm:grid-cols-2">
        <label>
          <span className="label m-0 block">Column to predict</span>
          <select className="w-full" value={choice.target} onChange={(e) => set({ target: e.target.value, value: valuesOf(columns, e.target.value)[0] ?? "" })} data-testid="quickstart-target">
            {choice.target ? null : <option value="">Choose a column</option>}
            {targets.map((t) => (
              <option key={t.name} value={t.name}>
                {t.label}
              </option>
            ))}
          </select>
        </label>
        {values.length ? (
          <label>
            <span className="label m-0 block">Value to look for</span>
            <select className="w-full" value={choice.value} onChange={(e) => set({ value: e.target.value })} data-testid="quickstart-target-value">
              {values.map((v) => (
                <option key={v} value={v}>
                  {v}
                </option>
              ))}
            </select>
          </label>
        ) : null}
        <label>
          <span className="label m-0 block">Column that names a row</span>
          <select className="w-full" value={choice.entity} onChange={(e) => set({ entity: e.target.value })} data-testid="quickstart-entity">
            <option value="">None: Lake numbers the rows</option>
            {(known ? columns : names.map((name) => ({ name, kind: null }))).filter((c) => c.name !== choice.target).map((c) => (
              <option key={c.name} value={c.name}>
                {c.kind === "identifier" ? `${c.name} (identifier)` : c.name}
              </option>
            ))}
          </select>
        </label>
        {known ? (
          <label>
            <span className="label m-0 block">Snapshot date</span>
            <input type="date" className="w-full" value={choice.asOf} onChange={(e) => set({ asOf: e.target.value })} data-testid="quickstart-as-of" />
          </label>
        ) : null}
      </div>
      {values.length ? (
        <p className="mb-0 text-[12.5px] text-ink-2">
          The model answers how likely a row's {choice.target} is {choice.value || "the value you choose"}.
        </p>
      ) : null}
      {known ? <ColumnList columns={columns} choice={choice} /> : null}
      <div className="mt-2">
        <CostNote costs={costs} step="model" />
      </div>
      {isBlocked(error) || blocked ? (
        <TopUp customerId={status.customer_id} />
      ) : (
        <>
          {error ? <ErrorBlock error={error} /> : null}
          <div className="mt-3">
            <Button variant={view === "failed" ? "secondary" : "primary"} loading={busy} disabled={!choice.target} onClick={() => train(known ? trainInput(choice, columns, status.snapshot_date) : { target: choice.target, ...(choice.entity ? { entity_column: choice.entity } : {}) })} data-testid="quickstart-train-button">
              <Brain size={14} aria-hidden /> {trainLabel(view === "failed")}
            </Button>
          </div>
        </>
      )}
    </>
  );

  const body = (
    <div data-testid="quickstart-train" data-view={view}>
      {view === "clustered" ? (
        <div data-testid="quickstart-clustered">
          <p className="m-0 text-[13px] text-ink-2">
            The clustering model is trained: it grouped the rows of {dataset} into segments. A clustering model answers no question per search result, so it is not added to your website; your search is ready on the website step above.
          </p>
          <div className="mt-3 flex flex-wrap gap-2">
            <Button variant="primary" to={segmentsPath} data-testid="quickstart-see-segments">
              <Brain size={14} aria-hidden /> See the segments
            </Button>
          </div>
        </div>
      ) : view === "done" ? (
        <div data-testid="quickstart-done">
          <p className="m-0 text-[13px] text-ink-2">
            The model is trained and published: the search above and your website code show how likely {asked ?? "each result"} is for each result. Next, copy the agent prompt or the code from the website step and put it on your site. <Link to="/ship/models">Ship → Models</Link> has its scores and traffic.
          </p>
        </div>
      ) : view === "publishing" ? (
        <p className="m-0 text-[13px] text-ink-2" data-testid="quickstart-publishing">
          <Brain size={14} aria-hidden className="inline" /> Trained. Publishing it for your website…
        </p>
      ) : view === "training" ? (
        <div data-testid="quickstart-training">
          <p className="m-0 text-[13px] text-ink-2">
            <Brain size={14} aria-hidden className="inline" /> Training {clustering ? "a clustering model" : asked ? `a model that predicts ${asked}` : "a model"} on {dataset}
            {since ? ` (${since} so far)` : ""}. This page follows it; you can leave and come back.
          </p>
          <p className="mb-0 mt-1 text-[12.5px] text-ink-3">
            {clustering ? "When it finishes, this step shows where to see the segments." : "When it finishes, Lake publishes the model, and the search above shows its answer beside each result."}
          </p>
        </div>
      ) : view === "failed" ? (
        <div data-testid="quickstart-failed">
          <p className="m-0 text-[13px] text-bad" role="alert" data-testid="quickstart-model-problem">
            {failureReason(status)}
          </p>
          {isBlocked(error) || blocked ? (
            <TopUp customerId={status.customer_id} />
          ) : (
            <>
              {error ? <ErrorBlock error={error} /> : null}
              <div className="mt-3 flex flex-wrap gap-2">
                <Button variant="primary" loading={busy} onClick={() => train({})} data-testid="quickstart-try-again">
                  <RotateCcw size={14} aria-hidden /> Try again
                </Button>
                <Button loading={asking} onClick={() => void askToFix()} data-testid="quickstart-ask-fix">
                  <MessageSquare size={14} aria-hidden /> Ask Orama to fix it
                </Button>
              </div>
            </>
          )}
          {!clustering && targets.length ? (
            <details className="mt-3 text-[12.5px]" data-testid="quickstart-change-choices">
              <summary className="cursor-pointer text-ink-2">Change the choices</summary>
              {form}
            </details>
          ) : null}
        </div>
      ) : known && targets.length === 0 ? (
        <p className="m-0 text-[12.5px] text-ink-2" data-testid="quickstart-no-target">
          No column here is a yes-or-no answer or a short category, so there is no column to pick. Ask Orama above can derive one (such as a price above a threshold), group the rows with clustering, or suggest another way to start.
        </p>
      ) : (
        form
      )}
      {(view === "training" || view === "done" || view === "clustered" || view === "publishing") && preparation ? <Prepared preparation={preparation} /> : null}
    </div>
  );
  if (bare) return body;
  const badge = view === "done" ? "published" : view === "clustered" ? "trained" : view === "publishing" ? "trained" : model?.state ?? "pending";
  return (
    <div data-testid="quickstart-train-card">
      <Card label="Step 4" title={clustering ? "Group the Rows into Segments" : "Train a Model on the Same Data"} tone={view === "done" || view === "clustered" ? "default" : view === "failed" ? "exception" : "active"} actions={<Status value={badge} tone={view === "done" || view === "clustered" ? "success" : stepTone(model?.state ?? "pending")} />}>
        {body}
      </Card>
    </div>
  );
}

/** What Lake does with each column for these choices: predicted, names a row, learned from, or left
 * out with why. Dates are read as of the snapshot date. */
function ColumnList({ columns, choice }: { columns: Column[]; choice: Choice }) {
  const uses = columnUses(columns, choice);
  return (
    <details className="mt-2 text-[12.5px]" data-testid="quickstart-columns">
      <summary className="cursor-pointer text-ink-2">
        How Lake reads your columns: {uses.filter((u) => u.used).length} used, {uses.filter((u) => !u.used).length} left out
      </summary>
      <ul className="mt-1 mb-0 grid list-none gap-0.5 p-0">
        {uses.map((u) => (
          <li key={u.name} data-testid="quickstart-column" data-use={u.use}>
            <span className="text-ink">{u.name}</span> <span className="text-ink-3">({u.kind})</span>: {u.use}
            {u.reason ? <span className="text-ink-3">, {u.reason}</span> : null}
          </li>
        ))}
      </ul>
    </details>
  );
}

/** What the training ran on, once it started. */
function Prepared({ preparation }: { preparation: NonNullable<NonNullable<QuickstartStatus["training"]>["preparation"]> }) {
  const left = preparation.dropped ?? [];
  return (
    <p className="mb-0 mt-2 text-[12px] text-ink-3" data-testid="quickstart-prepared">
      Learned from {(preparation.used ?? []).join(", ") || "no other column"}.
      {left.length ? ` Left out: ${left.map((d) => d.name).join(", ")}.` : ""}
      {preparation.note ? ` ${preparation.note}` : ""}
      {preparation.copy && preparation.dataset_id ? ` Trained on the copy ${preparation.dataset_id}.` : ""}
    </p>
  );
}
