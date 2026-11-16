// SPDX-License-Identifier: MIT

/** Design → Methodology (lake#655): one methodology, Orama's or the workspace's own.
 *
 * Orama's is read-only and marked "By Orama": what it needs, makes and is measured by, the
 * solutions that use it, a way to build a pipeline with it when it runs, and Duplicate to Edit.
 *
 * Your own is Python in a workspace repository: its status (usable only once Validate passed for
 * its current code), Validate's findings, its files (edited here and committed to the repository),
 * its runs, and the sandbox it runs in. Validate, Train, Edit, Duplicate and Delete are in the
 * header. */
import { useState, type ReactNode } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { CheckCircle2, Copy, FlaskConical, Pencil, Play, ShieldCheck, Trash2, Workflow, XCircle } from "lucide-react";
import { act } from "@/lib/api";
import { ConfirmButton, Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Badge, Button, Card, Chip, DataTable, Empty, ErrorBlock, Field, Form, Grid, KeyValue, Page, Skeleton, Split, StateTabs, fieldError } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { MethodologyIcon, SolutionIcon } from "@/components/CatalogueIcon";
import { qs, useScope, useScopedRead, useWq, useWsPath } from "./api";
import { datasetChoices, datasetColumns, joined, solutionUses, text, type J } from "./logic";
import {
  declaredParameters,
  heldOutMetrics,
  methodologyBadge,
  methodologySource,
  needsTarget,
  orderedFiles,
  parseParams,
  runIsLive,
  runPhaseTone,
  runQueuedReason,
  sortedFindings,
  validationState,
} from "./methodologies";
import { FormError, Panel, RunsBadge, When, entityCrumb, useDesignTrail, useSubmit } from "./parts";
import { NO_METHODOLOGIES } from "./MethodologiesPage";

type Open = "train" | "edit" | null;

export default function MethodologyPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  const ready = useWorkspaceStatus().q.data?.ready === true;
  // The same read the body makes: the header names the methodology and its actions.
  const got = useScopedRead<{ methodology?: J }>("get_methodology", { id }, { retry: false, enabled: ready && !!id });
  const entry = got.data?.methodology ?? null;
  const own = !!entry && methodologySource(entry) === "workspace";
  const name = entry ? text(entry, "name") || id : "";
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [id ? entityCrumb(own ? name : id) : { label: "Methodology" }], id ? `Methodology: ${own ? name : id}` : "Methodology");
  const [open, setOpen] = useState<Open>(null);
  return (
    <Page
      eyebrow={entry ? (own ? "Design · Your Methodology" : `Design · Methodology ${id}`) : "Design · Methodology"}
      title={
        entry ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {own ? name : `${name}`}
            <Badge tone={own ? "success" : "info"} title={own ? "Your workspace's own, in Python: you can edit it" : "Orama's, built into the engine: read-only"}>
              {methodologyBadge(entry)}
            </Badge>
            {own ? <Chip tone={validationState(entry).tone}>{validationState(entry).label}</Chip> : <RunsBadge entry={entry} />}
          </span>
        ) : (
          id || "Methodology"
        )
      }
      sub={entry && own ? text(entry, "description") : undefined}
      icon={entry ? <MethodologyIcon entry={entry} size={64} /> : undefined}
      actions={entry ? <HeaderActions entry={entry} own={own} onOpen={setOpen} /> : undefined}
    >
      <WorkspaceGate>
        <Body key={id} id={id} open={open} onOpen={setOpen} />
      </WorkspaceGate>
    </Page>
  );
}

function HeaderActions({ entry, own, onOpen }: { entry: J; own: boolean; onOpen: (open: Open) => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [busy, setBusy] = useState<string | null>(null);
  const id = text(entry, "id");
  const scoped = { customer_id: scope.customerId, workspace_id: scope.workspaceId };
  const duplicate = async () => {
    setBusy("duplicate");
    try {
      const answer = await act<{ methodology?: J }>("duplicate_methodology", { ...scoped, id });
      toast(`Duplicated as ${text(answer.methodology, "name")}`);
      navigate(ws(`/design/methodology?${qs({ id: text(answer.methodology, "id") })}`));
    } catch (e) {
      toast((e as Error).message, "bad");
    } finally {
      setBusy(null);
    }
  };
  const validate = async () => {
    setBusy("validate");
    try {
      const answer = await act<{ ready?: boolean }>("validate_methodology", { ...scoped, id });
      toast(answer.ready ? "Validating: its interface runs in the sandbox on a sample" : "Validate found something missing", answer.ready ? undefined : "bad");
    } catch (e) {
      toast((e as Error).message, "bad");
    } finally {
      setBusy(null);
    }
  };
  if (!own) {
    return (
      <Button loading={busy === "duplicate"} onClick={() => void duplicate()}>
        <Copy size={14} aria-hidden /> Duplicate to edit
      </Button>
    );
  }
  const usable = validationState(entry).label === "Usable";
  return (
    <>
      <Button onClick={() => onOpen("edit")}>
        <Pencil size={14} aria-hidden /> Edit
      </Button>
      <Button loading={busy === "duplicate"} onClick={() => void duplicate()}>
        <Copy size={14} aria-hidden /> Duplicate
      </Button>
      <ConfirmButton
        variant="ghost"
        danger
        title="Delete This Methodology?"
        confirmLabel="Delete methodology"
        typed={text(entry, "name")}
        confirm={<>{text(entry, "name")} leaves this workspace's registry and its runs stop. Its repository, its models and its run records are kept.</>}
        onConfirm={async () => {
          await act("delete_methodology", { ...scoped, id });
          toast(`Deleted ${text(entry, "name")}`);
          navigate(ws("/design/methodologies"));
        }}
      >
        <Trash2 size={14} aria-hidden /> Delete
      </ConfirmButton>
      <Button variant={usable ? "secondary" : "primary"} loading={busy === "validate"} onClick={() => void validate()}>
        <ShieldCheck size={14} aria-hidden /> Validate
      </Button>
      <Button variant={usable ? "primary" : "secondary"} disabled={!usable} title={usable ? undefined : "Validate it first"} onClick={() => onOpen("train")}>
        <Play size={14} aria-hidden /> Train
      </Button>
    </>
  );
}

function Body({ id, open, onOpen }: { id: string; open: Open; onOpen: (open: Open) => void }) {
  const got = useScopedRead<{ methodology?: J; source?: J }>("get_methodology", { id, include_source: true }, { retry: false, enabled: !!id });
  if (got.isLoading) return <Skeleton />;
  if (got.isError && (got.error as { code?: string }).code === "not_found")
    return (
      <Empty icon={FlaskConical} title={`No Methodology ${id}`}>
        Neither Orama's catalogue nor this workspace has a methodology by that id.
      </Empty>
    );
  if (got.isError && (got.error as { code?: string }).code === "unknown_capability") return <OramaFromList id={id} />;
  if (got.isError) return <ErrorBlock error={got.error} retry={() => void got.refetch()} what="this methodology" />;
  const entry = got.data?.methodology;
  if (!entry) return <Empty title={`No Methodology ${id}`}>This workspace's engine lists no methodology by that id.</Empty>;
  if (methodologySource(entry) === "workspace") return <OwnMethodology entry={entry} source={got.data?.source ?? {}} open={open} onOpen={onOpen} refetch={() => void got.refetch()} />;
  return <OramaMethodology entry={entry} />;
}

/** An engine without get_methodology still lists its catalogue: Orama's read from the list. */
function OramaFromList({ id }: { id: string }) {
  const methodologies = useWq<J>("list_methodologies", {}, { retry: false });
  if (methodologies.isLoading) return <Skeleton />;
  const entries: J[] = !methodologies.isError && Array.isArray(methodologies.data?.methodologies) ? methodologies.data!.methodologies : [];
  if (!entries.length)
    return (
      <Empty icon={FlaskConical} title="No Methodology Registry Yet">
        {NO_METHODOLOGIES}
      </Empty>
    );
  const entry = entries.find((e) => text(e, "id") === id);
  if (!entry) return <Empty title={`No Methodology ${id}`}>The engine lists no methodology by that id.</Empty>;
  return <OramaMethodology entry={entry} />;
}

function OramaMethodology({ entry }: { entry: J }) {
  const ws = useWsPath();
  const id = text(entry, "id");
  const solutions = useScopedRead<{ solutions?: J[] }>("list_solutions", {}, { retry: false });
  const users: J[] = (Array.isArray(solutions.data?.solutions) ? solutions.data!.solutions! : []).filter((s: J) => solutionUses(s, id));
  const runnable = entry.runnable === true;
  return (
    <Split
      side={
        <>
          <Panel label="Status" title={<RunsBadge entry={entry} />}>
            <KeyValue
              rows={[
                ["From", "Orama, built into the engine, read-only"],
                ["Family", text(entry, "family")],
                ["Solutions", String(users.length)],
                ...(!runnable && text(entry, "issue") ? ([["Tracked in", text(entry, "issue")]] as [string, string][]) : []),
              ]}
            />
          </Panel>
          {runnable && (
            <Panel title="Build With It">
              <p className="mt-0 text-[13px] text-ink-2">Open a project and choose {id} under Build a Pipeline.</p>
              <Button variant="primary" to={ws("/design/projects")}>
                <Workflow size={14} aria-hidden /> Build a pipeline with it
              </Button>
            </Panel>
          )}
          <Panel title="Make It Your Own">
            <p className="mt-0 text-[13px] text-ink-2">Orama's methodologies are read-only. Duplicate this one to start one of your own in Python: its description and parameters are copied, with a first version of the code to change.</p>
          </Panel>
        </>
      }
    >
      <Panel label={id} title={text(entry, "name")}>
        <KeyValue
          rows={[
            ["What it needs", joined(entry.inputs)],
            ["What it makes", joined(entry.outputs)],
            ["How it is measured", joined(entry.metrics)],
            ["Model kinds", joined(entry.model_kinds)],
          ]}
        />
      </Panel>
      <Panel title="Solutions That Use It">
        {users.length ? (
          <Grid className="grid-cols-1 gap-2 sm:grid-cols-2">
            {users.map((s, index) => (
              <Card key={text(s, "id") || index} to={ws(`/design/solution?${qs({ id: text(s, "id") })}`)} icon={<SolutionIcon entry={s} size={32} />} label={text(s, "id")} title={text(s, "name")} actions={<RunsBadge entry={s} />} />
            ))}
          </Grid>
        ) : (
          <p className="m-0 text-[13px] text-ink-3">—</p>
        )}
      </Panel>
    </Split>
  );
}

function OwnMethodology({ entry, source, open, onOpen, refetch }: { entry: J; source: J; open: Open; onOpen: (open: Open) => void; refetch: () => void }) {
  const ws = useWsPath();
  const id = text(entry, "id");
  const [live, setLive] = useState(false);
  const runs = useScopedRead<{ runs?: J[] }>("list_methodology_runs", { methodology_id: id, limit: 20 }, { refetchInterval: live ? 4000 : false });
  const all: J[] = Array.isArray(runs.data?.runs) ? runs.data!.runs! : [];
  const nowLive = all.some(runIsLive);
  if (nowLive !== live) {
    setLive(nowLive);
    if (!nowLive) refetch();
  }
  const state = validationState(entry);
  const validation: J = entry.validation ?? {};
  const findings = sortedFindings(source.findings?.length ? [...(validation.findings ?? []), ...source.findings] : validation.findings);
  const resources: J = entry.resources ?? {};
  const parameters = declaredParameters(entry);
  return (
    <>
      <Split
        side={
          <>
            <Panel label="Status" title={<Chip tone={state.tone}>{state.label}</Chip>}>
              <p className="mt-0 text-[13px] text-ink-2">{state.detail}</p>
              <KeyValue
                rows={[
                  ["From", "Your workspace, in Python"],
                  ["Repository", text(source, "repository_name") || text(entry.code, "repository_id")],
                  ["Revision", <code key="r">{text(entry.code, "revision").slice(0, 16) || "—"}</code>],
                  ...(text(entry, "duplicated_from") ? ([["Copied from", text(entry, "duplicated_from")]] as [string, ReactNode][]) : []),
                ]}
              />
            </Panel>
            <Panel title="Its Sandbox">
              <p className="mt-0 text-[13px] text-ink-2">Each run is untrusted code in its own container: its pinned packages, no credentials of yours or of the workspace, and the data handed in and the result taken out only through Lake.</p>
              <KeyValue
                rows={[
                  ["CPU", text(resources, "cpu") || "1"],
                  ["Memory", text(resources, "memory") || "2Gi"],
                  ["GPUs", String(resources.gpu ?? 0)],
                  ["Time limit", `${resources.timeout_seconds ?? 3600} s`],
                  ["Network", entry.network === true ? "Reaches the internet" : "None"],
                ]}
              />
            </Panel>
            {state.label === "Usable" && (
              <Panel title="Use It">
                <p className="mt-0 text-[13px] text-ink-2">Train it here, choose {id} under Build a Pipeline in a project, or name it in one of your solutions.</p>
                <Button to={ws("/design/projects")}>
                  <Workflow size={14} aria-hidden /> Build a pipeline with it
                </Button>
              </Panel>
            )}
          </>
        }
      >
        <Panel title="What It Is">
          <KeyValue
            rows={[
              ["Reads", joined(entry.interface?.input_kinds ?? entry.input_kinds)],
              ["Target", entry.interface?.target?.kind === "none" || !needsTarget(entry) ? "None" : text(entry.interface?.target ?? {}, "kind")],
              ["Reports", joined(entry.interface?.metrics ?? entry.metrics)],
              ["Parameters", parameters.map((p) => `${p.name} (${p.kind}, ${String(p.default)})`).join(", ") || "—"],
            ]}
          />
        </Panel>
        <Panel title="Validate" label={validation.validated_at ? <When value={String(validation.validated_at)} /> : undefined}>
          {findings.length ? (
            <ul className="m-0 list-none space-y-1.5 p-0">
              {findings.map((finding, index) => (
                <li key={index} className="flex items-start gap-2 text-[13px]">
                  {finding.ok === true ? <CheckCircle2 size={15} className="mt-[2px] shrink-0 text-ok" aria-label="Passed" /> : <XCircle size={15} className="mt-[2px] shrink-0 text-bad" aria-label="Failed" />}
                  <span>
                    <code className="text-[12px]">{text(finding, "check")}</code> {text(finding, "message")}
                  </span>
                </li>
              ))}
            </ul>
          ) : (
            <p className="m-0 text-[13px] text-ink-3">Not validated yet. Validate checks the manifest, fit, predict and score, and the lock file, then runs it on a sample in its sandbox.</p>
          )}
        </Panel>
        <CodePanel entry={entry} source={source} onSaved={refetch} />
        <Panel title="Runs">
          <DataTable<J>
            rows={all}
            q={runs}
            what="runs"
            empty="No runs yet. Validate runs it on a sample; Train runs it on a dataset."
            rowKey={(run) => text(run, "id")}
            columns={[
              { key: "mode", label: "Run", render: (run) => (text(run, "mode") === "train" ? "Train" : "Validate") },
              {
                key: "phase",
                label: "State",
                render: (run) => (
                  <>
                    <Chip tone={runPhaseTone(run)}>{text(run, "phase")}</Chip>
                    {runQueuedReason(run) && <div className="mt-1 max-w-[280px] text-[12px] text-ink-3">{runQueuedReason(run)}</div>}
                  </>
                ),
              },
              { key: "dataset", label: "Data", render: (run) => text(run, "dataset") || (text(run, "mode") === "validate" ? "Sample" : "—") },
              {
                key: "metrics",
                label: "Held out",
                render: (run) =>
                  heldOutMetrics(run)
                    .map(([name, value]) => `${name.slice(9)} ${value.toPrecision(3)}`)
                    .join(", ") || "—",
              },
              { key: "model", label: "Model", render: (run) => (text(run, "model_id") ? <Link to={ws(`/train/model?${qs({ id: text(run, "model_id") })}`)}>Open</Link> : "—") },
              { key: "created_at", label: "Started", render: (run) => <When value={text(run, "created_at")} />, sort: (run) => text(run, "created_at") },
            ]}
          />
          {all.some((run) => text(run, "message") && runIsLive(run) && !runQueuedReason(run)) && (
            <p className="mb-0 mt-2 text-[12.5px] text-ink-3">{text(all.find((run) => runIsLive(run) && !runQueuedReason(run)) ?? {}, "message")}</p>
          )}
        </Panel>
      </Split>
      {open === "train" && <TrainDrawer entry={entry} parameters={parameters} onClose={() => onOpen(null)} onStarted={() => setLive(true)} />}
      {open === "edit" && <EditDrawer entry={entry} onClose={() => onOpen(null)} onSaved={refetch} />}
    </>
  );
}

/** The methodology's files at its revision, one tab each; Save commits the edited file. */
function CodePanel({ entry, source, onSaved }: { entry: J; source: J; onSaved: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const files = orderedFiles(source.files);
  const [path, setPath] = useState<string>(files[0]?.path ?? "");
  const [edits, setEdits] = useState<Record<string, string>>({});
  const { busy, error, run } = useSubmit();
  if (source.available !== true) {
    return (
      <Panel title="Code">
        <p className="m-0 text-[13px] text-ink-3">{text(source, "message") || "Its files could not be read."}</p>
      </Panel>
    );
  }
  const current = files.find((f) => f.path === path) ?? files[0];
  const value = current ? (edits[current.path] ?? text(current, "contents")) : "";
  const changed = Object.keys(edits).filter((p) => edits[p] !== text(files.find((f) => f.path === p) ?? {}, "contents"));
  const save = () =>
    run(async () => {
      await act("update_methodology", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        id: text(entry, "id"),
        files: changed.map((p) => ({ path: p, contents: edits[p] })),
        message: `Edit ${changed.join(", ")}`,
      });
      setEdits({});
      toast("Committed. Validate it again before it runs.");
      onSaved();
    });
  return (
    <Panel title="Code" label={`${files.length} files`}>
      {files.length > 1 && <StateTabs<string> value={current?.path ?? ""} onChange={setPath} label="Files" tabs={files.map((f) => ({ value: f.path, label: <span className="font-mono text-[12px]">{f.path}</span> }))} />}
      {current && (
        <Form id="methodology-code" onSubmit={save}>
          <FormError error={error} />
          <textarea
            aria-label={current.path}
            spellCheck={false}
            className="mt-2 min-h-72 w-full rounded-md border border-rule bg-surface-2 p-2 font-mono text-[12px] leading-5"
            value={value}
            onChange={(e) => setEdits({ ...edits, [current.path]: e.target.value })}
          />
          <div className="mt-2 flex flex-wrap items-center gap-2">
            <Button type="submit" form="methodology-code" variant="primary" loading={busy} disabled={!changed.length}>
              Commit {changed.length ? `${changed.length} file${changed.length === 1 ? "" : "s"}` : "changes"}
            </Button>
            <span className="text-[12.5px] text-ink-3">Commits to its repository; a new revision is validated again before it runs.</span>
          </div>
        </Form>
      )}
    </Panel>
  );
}

function TrainDrawer({ entry, parameters, onClose, onStarted }: { entry: J; parameters: J[]; onClose: () => void; onStarted: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const datasets = useWq<J>("list_datasets");
  const choices = datasetChoices(datasets.data?.datasets);
  const [dataset, setDataset] = useState("");
  const [target, setTarget] = useState("");
  const [holdout, setHoldout] = useState("0.2");
  const [values, setValues] = useState<Record<string, string>>({});
  const columns = datasetColumns(datasets.data?.datasets, dataset);
  const { busy, error, setError, run } = useSubmit();
  const submit = () =>
    run(async () => {
      let params: Record<string, unknown>;
      try {
        params = parseParams(parameters, values);
      } catch (e) {
        setError(e);
        throw e;
      }
      await act("run_methodology", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        id: text(entry, "id"),
        dataset,
        ...(target ? { target } : {}),
        params,
        holdout_fraction: Number(holdout) || 0.2,
      });
      toast("Training: its code runs in its sandbox");
      onStarted();
      onClose();
    });
  return (
    <Drawer
      title="Train"
      subtitle="Runs its code on a dataset in its sandbox. The model it trains appears in Train → Models."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="train-methodology" variant="primary" loading={busy} disabled={!dataset || (needsTarget(entry) && !target)}>
            <Play size={14} aria-hidden /> Train
          </Button>
        </>
      }
    >
      <Form id="train-methodology" onSubmit={submit}>
        <FormError error={error} />
        <Field label="Dataset" required error={fieldError(error, "dataset")}>
          <select name="dataset" value={dataset} onChange={(e) => (setDataset(e.target.value), setTarget(""))}>
            <option value="">Choose a dataset</option>
            {choices.map((d) => (
              <option key={d.path} value={d.path}>
                {d.name}
              </option>
            ))}
          </select>
        </Field>
        {needsTarget(entry) && (
          <Field label="Target column" required help="What it learns to predict; every other column is a feature." error={fieldError(error, "target")}>
            {columns.length ? (
              <select name="target" value={target} onChange={(e) => setTarget(e.target.value)}>
                <option value="">Choose a column</option>
                {columns.map((c) => (
                  <option key={c}>{c}</option>
                ))}
              </select>
            ) : (
              <input name="target" value={target} placeholder="churned" onChange={(e) => setTarget(e.target.value)} />
            )}
          </Field>
        )}
        <Field label="Held out" help="The share of rows kept out of training to measure it honestly.">
          <input name="holdout_fraction" inputMode="decimal" value={holdout} onChange={(e) => setHoldout(e.target.value)} />
        </Field>
        {parameters.length > 0 && <div className="label mt-2">Parameters</div>}
        {parameters.map((p) => (
          <Field key={String(p.name)} label={String(p.name)} help={`${p.kind}, default ${String(p.default)}${p.minimum !== undefined ? `, from ${p.minimum}` : ""}${p.maximum !== undefined ? ` to ${p.maximum}` : ""}`} error={fieldError(error, `params/${p.name}`)}>
            {p.kind === "categorical" || p.kind === "bool" ? (
              <select name={String(p.name)} value={values[p.name] ?? ""} onChange={(e) => setValues({ ...values, [p.name]: e.target.value })}>
                <option value="">Default ({String(p.default)})</option>
                {(p.kind === "bool" ? ["true", "false"] : (p.choices ?? []).map(String)).map((c: string) => (
                  <option key={c}>{c}</option>
                ))}
              </select>
            ) : (
              <input name={String(p.name)} inputMode="decimal" placeholder={String(p.default)} value={values[p.name] ?? ""} onChange={(e) => setValues({ ...values, [p.name]: e.target.value })} />
            )}
          </Field>
        ))}
      </Form>
    </Drawer>
  );
}

function EditDrawer({ entry, onClose, onSaved }: { entry: J; onClose: () => void; onSaved: () => void }) {
  const scope = useScope();
  const [name, setName] = useState(text(entry, "name"));
  const [description, setDescription] = useState(text(entry, "description"));
  const [network, setNetwork] = useState(entry.network === true);
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const input: Record<string, unknown> = { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(entry, "id") };
      if (name !== text(entry, "name")) input.name = name;
      if (description !== text(entry, "description")) input.description = description;
      if (network !== (entry.network === true)) input.network = network;
      await act("update_methodology", input);
      onSaved();
      onClose();
    });
  return (
    <Drawer
      title="Edit Methodology"
      subtitle="The name and description are its manifest's, committed to its repository."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="edit-methodology" variant="primary" loading={busy}>
            Save
          </Button>
        </>
      }
    >
      <Form id="edit-methodology" onSubmit={submit}>
        <FormError error={fieldError(error, "name") ? null : error} />
        <Field label="Name" required error={fieldError(error, "name")}>
          <input name="name" maxLength={200} value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Description">
          <textarea name="description" rows={4} value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <Field label="Network" help="Off by default: its runs reach nothing. On, they reach the internet, never Lake, the workspace or the cluster.">
          <label className="inline-flex items-center gap-2 text-[13px]">
            <input type="checkbox" name="network" checked={network} onChange={(e) => setNetwork(e.target.checked)} /> Its runs may reach the internet
          </label>
        </Field>
      </Form>
    </Drawer>
  );
}
