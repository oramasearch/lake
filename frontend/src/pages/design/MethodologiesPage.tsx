/** Design → Methodologies (lake#655): Orama's methodologies, read-only and marked "By Orama", and
 * the workspace's own, written in Python and kept in a workspace repository, marked "Yours". A
 * toolbar filters by who a methodology is from, by family and by a search; New Methodology opens a
 * drawer that drafts a first version from a description. One read, the engine's
 * `list_methodologies`, answers both kinds. */
import { useEffect, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { Code2, FlaskConical, Plus, SearchX, Sparkles } from "lucide-react";
import { act, postRead } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { Badge, Button, Card, Chip, Empty, FilterChips, Grid, Field, Form, Page, SearchInput, Section, Skeleton, Stat, StatRow, Toolbar, fieldError } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { MethodologyIcon } from "@/components/CatalogueIcon";
import { qs, useScope, useWq, useWsPath } from "./api";
import { joined, text, type J } from "./logic";
import { TARGET_KINDS, filterMethodologies, methodologyBadge, methodologySource, validationState, type MethodologySource } from "./methodologies";
import { FormError, RunsBadge, useDesignTrail, useSubmit } from "./parts";
import { taskReply, taskWorking, useAgentDeployed, useAgentTask } from "@/pages/apps/shared";
import { draftAuthor, type MethodologyDraft } from "./methodologies";

export const NO_METHODOLOGIES = "This workspace's engine does not list its methodologies yet. It runs tabular prediction (M-1).";

export default function MethodologiesPage() {
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [], "Methodologies");
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const [creating, setCreating] = useState(false);
  return (
    <Page
      eyebrow="Design"
      title="Methodologies"
      sub="The modelling methods your workspace can run: Orama's, built into the engine, and your own, written in Python. Solutions compose them, pipelines run them."
      actions={
        ready ? (
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New methodology
          </Button>
        ) : undefined
      }
    >
      <WorkspaceGate>
        <Methodologies onNew={() => setCreating(true)} />
        {creating && <NewMethodology onClose={() => setCreating(false)} />}
      </WorkspaceGate>
    </Page>
  );
}

function Methodologies({ onNew }: { onNew: () => void }) {
  const ws = useWsPath();
  const [search, setSearch] = useState("");
  const [source, setSource] = useState<MethodologySource | null>(null);
  const [family, setFamily] = useState<string | null>(null);
  // A failed read is not shown as an error: an older engine has no methodology registry.
  const listed = useWq<J>("list_methodologies", {}, { retry: false });
  if (listed.isLoading) return <Skeleton />;
  const entries: J[] = !listed.isError && Array.isArray(listed.data?.methodologies) ? listed.data!.methodologies : [];
  if (!entries.length) {
    return (
      <Empty icon={FlaskConical} title="No Methodology Registry Yet">
        {NO_METHODOLOGIES}
      </Empty>
    );
  }
  const orama = entries.filter((e) => methodologySource(e) === "orama");
  const yours = entries.filter((e) => methodologySource(e) === "workspace");
  const counts = new Map<string, number>();
  for (const e of orama) counts.set(text(e, "family"), (counts.get(text(e, "family")) ?? 0) + 1);
  const shown = filterMethodologies(entries, { source, family, search });
  const shownYours = shown.filter((e) => methodologySource(e) === "workspace");
  // Consecutive entries of one family share a section, as the engine lists them.
  const families: { family: string; entries: J[] }[] = [];
  for (const entry of shown.filter((e) => methodologySource(e) === "orama")) {
    const name = text(entry, "family");
    const last = families[families.length - 1];
    if (last && last.family === name) last.entries.push(entry);
    else families.push({ family: name, entries: [entry] });
  }
  const runnable = entries.filter((e) => e.runnable === true).length;
  const filtering = !!search.trim() || !!source || !!family;
  return (
    <>
      <StatRow cols={3}>
        <Stat label="By Orama" value={orama.length} sub="built into the engine, read-only" />
        <Stat label="Yours" value={yours.length} sub="written in Python in this workspace" />
        <Stat label="Run here" value={runnable} tone={runnable ? "ok" : undefined} sub="today, in this workspace" />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search methodologies…" label="Search methodologies" />
        <FilterChips<MethodologySource>
          allLabel="All"
          options={[
            { id: "orama", label: "By Orama", count: orama.length },
            { id: "workspace", label: "Yours", count: yours.length },
          ]}
          value={source}
          onChange={setSource}
        />
        {source !== "workspace" && <FilterChips options={[...counts].map(([id, count]) => ({ id, label: id || "—", count }))} value={family} onChange={setFamily} allLabel="All families" />}
      </Toolbar>
      {filtering && !shown.length && (
        <Empty compact icon={SearchX} title="No Methodology Matches">
          Clear the search or a filter to see more.
        </Empty>
      )}
      {(source === null || source === "workspace") && !family && (shownYours.length > 0 || !filtering) && (
        <Section title="Yours">
          {shownYours.length ? (
            <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
              {shownYours.map((entry, row) => {
                const state = validationState(entry);
                return (
                  <Card
                    key={text(entry, "id") || row}
                    to={ws(`/design/methodology?${qs({ id: text(entry, "id") })}`)}
                    icon={<MethodologyIcon entry={entry} size={32} />}
                    title={text(entry, "name")}
                    actions={
                      <span className="inline-flex flex-wrap items-center gap-1">
                        <Badge tone="success" title="Your workspace's own, in Python: you can edit it">
                          {methodologyBadge(entry)}
                        </Badge>
                        <Chip tone={state.tone}>{state.label}</Chip>
                      </span>
                    }
                    className="h-full"
                  >
                    <p className="m-0 line-clamp-3 text-[13px] text-ink-2">{text(entry, "description") || "—"}</p>
                    <div className="mt-2 text-[12.5px] text-ink-3">
                      <Code2 size={12} aria-hidden className="mr-1 inline" />
                      {joined(entry.interface?.input_kinds ?? entry.input_kinds) || "Python"}
                    </div>
                  </Card>
                );
              })}
            </Grid>
          ) : (
            <Empty
              compact
              icon={Code2}
              title="No Methodologies of Your Own Yet"
              action={
                <Button variant="primary" onClick={onNew}>
                  <Plus size={14} aria-hidden /> New methodology
                </Button>
              }
            >
              Write one in Python for a method Orama does not build in, or open one of Orama's and press Duplicate to edit.
            </Empty>
          )}
        </Section>
      )}
      {source !== "workspace" &&
        families.map((group, index) => (
          <Section key={index} title={group.family || "By Orama"}>
            <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
              {group.entries.map((entry, row) => (
                <Card
                  key={text(entry, "id") || row}
                  to={ws(`/design/methodology?${qs({ id: text(entry, "id") })}`)}
                  icon={<MethodologyIcon entry={entry} size={32} />}
                  label={text(entry, "id")}
                  title={text(entry, "name")}
                  actions={
                    <span className="inline-flex flex-wrap items-center gap-1">
                      <Badge tone="info" title="Orama's, built into the engine: read-only">
                        {methodologyBadge(entry)}
                      </Badge>
                      <RunsBadge entry={entry} />
                    </span>
                  }
                  className="h-full"
                >
                  <div className="text-[13px] text-ink-2">
                    <span className="text-ink-3">Makes </span>
                    {joined(entry.outputs) || "—"}
                  </div>
                  {joined(entry.inputs) && <div className="mt-1 line-clamp-2 text-[12.5px] text-ink-3">Needs {joined(entry.inputs)}</div>}
                </Card>
              ))}
            </Grid>
          </Section>
        ))}
    </>
  );
}

/** New Methodology: a name and what it should do. Ask Orama writes a first version for the
 * description (a `methodology_draft` turn of agent-ask-orama, lake#688), Lake checks it with the
 * checks Validate runs first and offers it for review; when Ask Orama is not running, or its draft
 * does not pass, Lake's template is offered and the drawer says why. Create commits the files to a
 * new repository in the workspace, and the methodology page opens. */
function NewMethodology({ onClose }: { onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const askOrama = useAgentDeployed("agent-ask-orama");
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [targetKind, setTargetKind] = useState("");
  const [draft, setDraft] = useState<MethodologyDraft | null>(null);
  const [drafting, setDrafting] = useState(false);
  const [draftError, setDraftError] = useState<unknown>(null);
  const [taskId, setTaskId] = useState("");
  const { busy, error, run } = useSubmit();
  const scoped = { customer_id: scope.customerId, workspace_id: scope.workspaceId };
  const draftInput = () => ({ ...scoped, description: description || name, ...(name ? { name } : {}), ...(targetKind ? { target_kind: targetKind } : {}) });
  const task = useAgentTask(taskId, scope.customerId, scope.workspaceId);
  const read = useRef("");
  // Ask Orama's turn finished: Lake checks its files and answers the draft it may offer.
  useEffect(() => {
    if (!taskId || !task.data || taskWorking(task.data) || read.current === taskId) return;
    read.current = taskId;
    postRead<{ draft?: MethodologyDraft }>("draft_methodology", { ...draftInput(), task_id: taskId })
      .then((answer) => setDraft(answer.draft ?? null))
      .catch((e: unknown) => setDraftError(e))
      .finally(() => {
        setDrafting(false);
        setTaskId("");
      });
    // draftInput reads the form as it was asked; the effect follows the task only.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [taskId, task.data]);
  const draftIt = async () => {
    setDrafting(true);
    setDraftError(null);
    try {
      if (askOrama.deployed) {
        const turn = await act<{ task_id?: string }>("agent_turn", {
          ...scoped,
          agent: "agent-ask-orama",
          kind: "methodology_draft",
          message: description || name,
          context: { ...(name ? { name } : {}), ...(targetKind ? { target_kind: targetKind } : {}) },
        });
        if (turn.task_id) {
          setTaskId(turn.task_id);
          return;
        }
      }
      const answer = await postRead<{ draft?: MethodologyDraft }>("draft_methodology", draftInput());
      setDraft(answer.draft ?? null);
      setDrafting(false);
    } catch (e) {
      setDraftError(e);
      setDrafting(false);
    }
  };
  const author = draft ? draftAuthor(draft, askOrama.deployed) : null;
  const submit = () =>
    run(async () => {
      const input: Record<string, unknown> = { ...scoped, name };
      if (draft?.files?.length) input.files = draft.files;
      else {
        input.description = description;
        if (targetKind) input.target_kind = targetKind;
      }
      const answer = await act<{ methodology?: J }>("create_methodology", input);
      navigate(ws(`/design/methodology?${qs({ id: text(answer.methodology, "id") })}`));
    });
  return (
    <Drawer
      title="New Methodology"
      subtitle="Python in a repository of this workspace: fit, predict and score, with its parameters and metrics."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button onClick={() => void draftIt()} loading={drafting} disabled={!name.trim() && !description.trim()}>
            <Sparkles size={14} aria-hidden /> Ask Orama
          </Button>
          <Button type="submit" form="new-methodology" variant="primary" loading={busy} disabled={!name.trim()}>
            Create methodology
          </Button>
        </>
      }
    >
      <Form id="new-methodology" onSubmit={submit}>
        <FormError error={fieldError(error, "name") ? null : error} />
        <Field label="Name" required error={fieldError(error, "name")}>
          <input name="name" maxLength={200} placeholder="Late payers" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="What it should do" help="Ask Orama writes a first version from it for you to review before you create it.">
          <textarea name="description" rows={3} placeholder="Predict which customers will pay their invoice late, from their history." value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <Field label="Its target">
          <select name="target_kind" value={targetKind} onChange={(e) => setTargetKind(e.target.value)}>
            {TARGET_KINDS.map((k) => (
              <option key={k.value} value={k.value}>
                {k.label}
              </option>
            ))}
          </select>
        </Field>
        <FormError error={draftError} />
        {drafting && taskId && (
          <p className="mt-2 text-[13px] text-ink-2" role="status">
            Ask Orama is writing a first version{task.data && taskReply(task.data) ? `: ${taskReply(task.data)}` : "."}
          </p>
        )}
        {draft?.files?.length && author ? (
          <div className="mt-2">
            <div className="flex flex-wrap items-center gap-2">
              <div className="label">The Draft</div>
              <Badge tone={author.tone}>{author.badge}</Badge>
            </div>
            <p className="mt-1 text-[13px] text-ink-2">
              {author.note ? `${author.note} ` : ""}
              {draft.reason} Create commits these files to a new private repository; edit them there or on the methodology's page, then Validate.
            </p>
            {author.failing.length > 0 && (
              <ul className="mt-1 list-disc pl-5 text-[12.5px] text-ink-3">
                {author.failing.map((finding, i) => (
                  <li key={i}>
                    <span className="font-mono">{finding.check}</span>: {finding.message}
                  </li>
                ))}
              </ul>
            )}
            {draft.files.map((file) => (
              <details key={file.path} className="mt-2" open={file.path === "methodology.py"}>
                <summary className="cursor-pointer font-mono text-[12.5px]">{file.path}</summary>
                <pre className="mt-1 max-h-72 overflow-auto rounded-md border border-rule bg-surface-2 p-2 text-[12px] leading-5">{file.contents || "(empty: the standard library only)"}</pre>
              </details>
            ))}
          </div>
        ) : (
          !drafting && <p className="mt-2 text-[12.5px] text-ink-3">Without a draft, Create starts from Lake's template for the description and commits it.</p>
        )}
      </Form>
    </Drawer>
  );
}
