/** Design → Solution (lake#597): one solution, Orama's or the workspace's own, read with lake's
 * `get_solution`. Orama's is read-only: its page shows the catalogue entry, its runs and Start This
 * Solution (a drawer) when it runs or is in preview, and offers Duplicate to Edit. Your own has
 * Edit, Duplicate and Delete (typed confirmation), and Start as a Project, which creates the project
 * and builds its first pipeline from the composed methodology, since the engine's solution
 * creation takes only a catalogue id. */
import { useState, type ReactNode } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Copy, Pencil, Rocket, Trash2 } from "lucide-react";
import { act } from "@/lib/api";
import { ConfirmButton, Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Badge, Button, Empty, ErrorBlock, Field, Form, KeyValue, Page, Skeleton, Split } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath, wa } from "./api";
import {
  datasetChoices,
  joined,
  parseInteger,
  parseNumber,
  pipelineReference,
  runIsActive,
  slugText,
  solutionForm,
  solutionInput,
  solutionMethodologies,
  solutionSource,
  startDecision,
  startIntent,
  text,
  type J,
} from "./logic";
import { MethodologyLinks, NoCatalogue } from "./catalogue";
import { FormError, Panel, Refusal, RunsBadge, RunsPanel, When, entityCrumb, useDesignTrail, useSubmit } from "./parts";
import { SolutionFields } from "./SolutionForm";
import "@/styles/design.css";

/** The solution in `get_solution`'s answer: {solution: {...}}, or the solution itself. */
function solutionOf(entry: J | undefined): J | null {
  if (!entry || (!("id" in entry) && !("solution" in entry))) return null;
  const solution: J = entry.solution && typeof entry.solution === "object" ? entry.solution : entry;
  return text(solution, "id") ? solution : null;
}

const levelsOf = (solution: J): J[] => (Array.isArray(solution.capability_levels) ? solution.capability_levels : Array.isArray(solution.levels) ? solution.levels : []);

type Open = "start" | "edit" | null;

export default function SolutionPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  // The same read the body makes (one request): the header names the solution and its actions.
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const got = useScopedRead<J>("get_solution", { id }, { retry: false, enabled: ready && !!id });
  const solution = solutionOf(got.data);
  const own = !!solution && solutionSource(solution) === "workspace";
  const name = solution ? text(solution, "name") || id : "";
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [id ? entityCrumb(own ? name : id) : { label: "Solution" }], id ? `Solution: ${own ? name : id}` : "Solution");
  const startable = !!solution && (own || solution.runnable === true || text(solution, "status") === "preview");
  const [open, setOpen] = useState<Open>(null);
  return (
    <Page
      eyebrow={solution ? (own ? "Design · Your Solution" : `Design · Solution ${text(solution, "id")}`) : "Design · Solution"}
      title={
        solution ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {name}
            <Badge tone={own ? "success" : "info"} title={own ? "Your workspace's own: you can edit it" : "Orama's catalogue: read-only"}>
              {own ? "Yours" : "Orama"}
            </Badge>
            {!own && <RunsBadge entry={solution} />}
          </span>
        ) : (
          id || "Solution"
        )
      }
      sub={solution ? (own ? text(solution, "decision") : text(solution, "promise")) : undefined}
      actions={solution ? <HeaderActions solution={solution} own={own} startable={startable} onOpen={setOpen} /> : undefined}
    >
      <WorkspaceGate>
        <SolutionBody key={id} id={id} open={open} onOpen={setOpen} />
      </WorkspaceGate>
    </Page>
  );
}

/** Orama's: Duplicate to Edit and Start. Yours: Start as a Project, Edit, Duplicate and Delete. */
function HeaderActions({ solution, own, startable, onOpen }: { solution: J; own: boolean; startable: boolean; onOpen: (open: Open) => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [busy, setBusy] = useState(false);
  const id = text(solution, "id");
  const duplicate = async () => {
    setBusy(true);
    try {
      const answer = await act<{ solution?: J }>("duplicate_solution", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id });
      toast(`Duplicated as ${text(answer.solution, "name")}`);
      navigate(ws(`/design/solution?${qs({ id: text(answer.solution, "id") })}`));
    } catch (e) {
      toast((e as Error).message, "bad");
    } finally {
      setBusy(false);
    }
  };
  if (!own) {
    return (
      <>
        <Button loading={busy} onClick={() => void duplicate()}>
          <Copy size={14} aria-hidden /> Duplicate to edit
        </Button>
        {startable && (
          <Button variant="primary" onClick={() => onOpen("start")}>
            <Rocket size={14} aria-hidden /> Start this solution
          </Button>
        )}
      </>
    );
  }
  return (
    <>
      <Button onClick={() => onOpen("edit")}>
        <Pencil size={14} aria-hidden /> Edit
      </Button>
      <Button loading={busy} onClick={() => void duplicate()}>
        <Copy size={14} aria-hidden /> Duplicate
      </Button>
      <ConfirmButton
        variant="ghost"
        danger
        title="Delete This Solution?"
        confirmLabel="Delete solution"
        typed={text(solution, "name")}
        confirm={<>The solution {text(solution, "name")} is deleted from this workspace and cannot be restored. Projects started from it are kept.</>}
        onConfirm={async () => {
          await act("delete_solution", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id });
          toast(`Deleted ${text(solution, "name")}`);
          navigate(ws("/design/solutions"));
        }}
      >
        <Trash2 size={14} aria-hidden /> Delete
      </ConfirmButton>
      <Button variant="primary" onClick={() => onOpen("start")}>
        <Rocket size={14} aria-hidden /> Start as a project
      </Button>
    </>
  );
}

function SolutionBody({ id, open, onOpen }: { id: string; open: Open; onOpen: (open: Open) => void }) {
  const got = useScopedRead<J>("get_solution", { id }, { retry: false, enabled: !!id });
  if (got.isLoading) return <Skeleton />;
  // An engine without the catalogue answers unknown_capability: the catalogue note, not an error.
  if (got.isError && (got.error as { code?: string }).code === "unknown_capability") return <NoCatalogue />;
  if (got.isError && (got.error as { code?: string }).code === "not_found")
    return <Empty title={`No Solution ${id}`}>Neither Orama's catalogue nor this workspace has a solution by that id.</Empty>;
  if (got.isError) return <ErrorBlock error={got.error} retry={() => void got.refetch()} what="this solution" />;
  const solution = solutionOf(got.data);
  if (!solution) return <Empty title={`No Solution ${id}`}>Neither Orama's catalogue nor this workspace has a solution by that id.</Empty>;
  if (solutionSource(solution) === "workspace") return <OwnSolution solution={solution} open={open} onOpen={onOpen} />;
  return <OramaSolution id={id} solution={solution} starting={open === "start"} onStarting={(on) => onOpen(on ? "start" : null)} />;
}

function OramaSolution({ id, solution, starting, onStarting }: { id: string; solution: J; starting: boolean; onStarting: (open: boolean) => void }) {
  const [live, setLive] = useState(false);
  const datasets = useWq<J>("list_datasets");
  const runs = useScopedRead<{ runs?: J[] }>("list_runs", { solution: id, limit: 5 }, { refetchInterval: live ? 3000 : false });
  const allRuns: J[] = Array.isArray(runs.data?.runs) ? runs.data!.runs! : [];
  const nowLive = allRuns.some(runIsActive);
  if (nowLive !== live) setLive(nowLive);
  const levels = levelsOf(solution);
  const runnable = solution.runnable === true;
  const startable = runnable || text(solution, "status") === "preview";
  return (
    <>
      <Split
        side={
          <>
            <Panel label="Status" title={<RunsBadge entry={solution} />}>
              <KeyValue
                rows={[
                  ["Id", <code key="id">{text(solution, "id")}</code>],
                  ["From", "Orama's catalogue, read-only"],
                  ["Catalogue", text(solution, "status")],
                  ...(!runnable && text(solution, "issue") ? ([["Tracked in", text(solution, "issue")]] as [string, string][]) : []),
                ]}
              />
            </Panel>
            {startable && (
              <Panel title="Start This Solution">
                <p className="mt-0 text-[13px] text-ink-2">You give the business inputs; the Project Agent proposes the rest of the foundation. Nothing is created until you confirm it.</p>
                <Button variant="primary" onClick={() => onStarting(true)}>
                  <Rocket size={14} aria-hidden /> Start
                </Button>
              </Panel>
            )}
            <Panel title="Make It Your Own">
              <p className="mt-0 text-[13px] text-ink-2">Orama's solutions are read-only. Duplicate this one to edit its problem, decision, measurement and methodologies in your workspace.</p>
            </Panel>
          </>
        }
      >
        <Panel label={text(solution, "id")} title="What It Does">
          <KeyValue
            rows={[
              ["The problem", text(solution, "problem") || text(solution, "use_case")],
              ["Methodologies", <MethodologyLinks key="m" solution={solution} />],
              ["How it acts", joined(solution.action_patterns)],
              ["Decision speed", text(solution, "latency") || joined(solution.latency_classes)],
              ["What data it needs", joined(Array.isArray(solution.required_data_roles) ? solution.required_data_roles : solution.data_roles)],
            ]}
          />
        </Panel>
        {levels.length > 0 && (
          <Panel title="Levels">
            <ol className="m-0 space-y-2 pl-0">
              {levels.map((level, index) => (
                <li key={index} className="flex list-none gap-3">
                  <span className="mt-0.5 flex h-5 w-5 shrink-0 items-center justify-center rounded-full bg-raised font-mono text-[11px] text-ink-2">{index + 1}</span>
                  <span className="text-[13px]">
                    <strong>{text(level, "name") || text(level, "id")}</strong> <span className="text-ink-2">{text(level, "description")}</span>
                  </span>
                </li>
              ))}
            </ol>
          </Panel>
        )}
        {runs.isError ? <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" /> : runs.isLoading ? <Skeleton /> : <RunsPanel runs={allRuns} />}
      </Split>
      {startable && starting && (
        <StartSolution
          solution={solution}
          levels={levels}
          datasets={datasetChoices(datasets.data?.datasets).map((d) => d.path)}
          datasetsError={datasets.error}
          onClose={() => onStarting(false)}
        />
      )}
    </>
  );
}

/** One of your own solutions: what it does and how its return is measured, where it came from,
 * and the Edit and Start as a Project drawers. */
function OwnSolution({ solution, open, onOpen }: { solution: J; open: Open; onOpen: (open: Open) => void }) {
  const ws = useWsPath();
  const m: J = solution.measurement && typeof solution.measurement === "object" ? solution.measurement : {};
  const pipeline = text(solution, "pipeline");
  const reference = pipelineReference(pipeline);
  const from = text(solution, "duplicated_from");
  return (
    <>
      <Split
        side={
          <>
            <Panel label="Source" title={<Badge tone="success">Yours</Badge>}>
              <KeyValue
                rows={[
                  ["From", "This workspace, editable"],
                  ...(from ? ([["Duplicated from", <Link key="from" to={ws(`/design/solution?${qs({ id: from })}`)}>{/^S-/.test(from) ? `Orama's ${from}` : "one of yours"}</Link>]] as [string, ReactNode][]) : []),
                  ["Created", <span key="c"><When value={text(solution, "created_at")} />{text(solution, "created_by_name") ? ` by ${text(solution, "created_by_name")}` : ""}</span>],
                  ...(text(solution, "updated_at") ? ([["Edited", <When key="u" value={text(solution, "updated_at")} />]] as [string, ReactNode][]) : []),
                ]}
              />
            </Panel>
            <Panel title="Start as a Project">
              <p className="mt-0 text-[13px] text-ink-2">Creates a project with this solution's decision and builds its first pipeline from the methodology it composes.</p>
              <Button variant="primary" onClick={() => onOpen("start")}>
                <Rocket size={14} aria-hidden /> Start as a project
              </Button>
            </Panel>
          </>
        }
      >
        <Panel title="What It Does">
          <KeyValue
            rows={[
              ["The problem", text(solution, "problem")],
              ["The decision it drives", text(solution, "decision")],
              ["Methodologies", <MethodologyLinks key="m" solution={solution} />],
              [
                "Pipeline",
                reference ? (
                  <Link key="p" to={ws(`/design/pipeline?${qs({ project: reference.project, graph: reference.graph })}`)}>
                    {pipeline}
                  </Link>
                ) : (
                  pipeline || "—"
                ),
              ],
            ]}
          />
        </Panel>
        <Panel title="How Its Return Is Measured">
          <KeyValue
            rows={[
              ["Metric", text(m, "metric")],
              ["Baseline", text(m, "baseline") || "—"],
              ["Target", text(m, "target") || "—"],
            ]}
          />
        </Panel>
        {text(solution, "notes") && (
          <Panel title="Notes">
            <p className="m-0 whitespace-pre-wrap text-[13px] text-ink-2">{text(solution, "notes")}</p>
          </Panel>
        )}
      </Split>
      {open === "edit" && <EditSolution solution={solution} onClose={() => onOpen(null)} />}
      {open === "start" && <StartOwnSolution solution={solution} onClose={() => onOpen(null)} />}
    </>
  );
}

function EditSolution({ solution, onClose }: { solution: J; onClose: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const [form, setForm] = useState(() => solutionForm(solution));
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      await act("update_solution", { customer_id: scope.customerId, workspace_id: scope.workspaceId, id: text(solution, "id"), ...solutionInput(form) });
      toast("Solution saved");
      onClose();
    });
  return (
    <Drawer
      title="Edit Solution"
      subtitle={text(solution, "name")}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="edit-solution" variant="primary" loading={busy}>
            Save
          </Button>
        </>
      }
    >
      <SolutionFields id="edit-solution" form={form} onChange={setForm} onSubmit={submit} error={error} />
    </Drawer>
  );
}

/** Start as a Project for one of your own solutions (lake#597). The engine's solution creation
 * takes only one of Orama's catalogue ids, so lake does what it can say honestly: it creates the
 * project with the solution's problem and decision (create_project), then builds its first
 * pipeline from the decision with the composed methodology chosen here (compile_pipeline). The
 * governed foundation (policy, holdout, ROI readout) of a catalogue solution is not created. */
function StartOwnSolution({ solution, onClose }: { solution: J; onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const datasets = useWq<J>("list_datasets");
  const composed = solutionMethodologies(solution);
  const [name, setName] = useState(() => slugText(text(solution, "name")));
  const [methodology, setMethodology] = useState(() => composed[0]?.id ?? "");
  const [dataset, setDataset] = useState("");
  const { busy, error, run } = useSubmit();
  const choices = datasetChoices(datasets.data?.datasets);
  const submit = () =>
    run(async () => {
      await wa(scope, "create_project", { name, description: text(solution, "problem"), decision: startDecision(solution) });
      try {
        const answer = await act<J>("compile_pipeline", {
          customer_id: scope.customerId,
          workspace_id: scope.workspaceId,
          project: name,
          intent: startIntent(solution),
          ...(dataset ? { dataset } : {}),
          ...(methodology ? { methodology } : {}),
        });
        const graph = text(answer, "graph");
        if (dataset && graph) {
          navigate(ws(`/design/pipeline?${qs({ project: name, graph, dataset })}#run`));
          return;
        }
        toast(graph ? `Project ${name} created with pipeline ${graph}` : `Project ${name} created`);
      } catch (e) {
        // The project exists; its first pipeline can be built again from the project page.
        toast(`Project ${name} created, but its first pipeline was not built: ${(e as Error).message}`, "bad");
      }
      navigate(ws(`/design/project?${qs({ name })}`));
    });
  return (
    <Drawer
      title="Start as a Project"
      subtitle={text(solution, "name")}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="start-own-solution" variant="primary" loading={busy}>
            Start
          </Button>
        </>
      }
    >
      <Form id="start-own-solution" onSubmit={submit}>
        <FormError error={error} />
        <p className="m-0 text-[13px] text-ink-2">
          lake creates the project with this solution's problem and decision, then builds its first pipeline from the decision. A solution of your own does not get the governed foundation (policy, holdout and ROI readout) that Orama's solutions do.
        </p>
        <Field label="Project name" required help="Lowercase letters, digits, - and _.">
          <input name="name" pattern="[a-z0-9][a-z0-9_\-]{0,62}" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Methodology" help={composed.length ? "The methodology its first pipeline uses." : "This solution composes no methodology; lake chooses from the decision."}>
          <select name="methodology" value={methodology} onChange={(e) => setMethodology(e.target.value)}>
            <option value="">Let lake choose from the decision</option>
            {composed.map((m) => (
              <option key={m.id} value={m.id}>
                {m.id}
                {m.level > 1 ? ` (from level ${m.level})` : ""}
              </option>
            ))}
          </select>
        </Field>
        <Field label="From data" help="Choose a dataset and the pipeline reads it." error={datasets.isError ? `The datasets could not be listed: ${(datasets.error as Error).message}` : undefined}>
          <select name="dataset" value={dataset} onChange={(e) => setDataset(e.target.value)}>
            <option value="">Let the builder describe its sources</option>
            {choices.map((d) => (
              <option key={d.path} value={d.path}>
                {d.name}
              </option>
            ))}
          </select>
        </Field>
      </Form>
    </Drawer>
  );
}

/** Start This Solution (lake#292): the person supplies the business inputs; the Project Agent
 * proposes the rest of the foundation, and nothing is created until it is confirmed. */
function StartSolution({ solution, levels, datasets, datasetsError, onClose }: { solution: J; levels: J[]; datasets: string[]; datasetsError: unknown; onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const levelValue = (level: J) => (typeof level.level === "number" ? String(Math.trunc(level.level)) : text(level, "id"));
  const [name, setName] = useState(() => slugText(text(solution, "name")));
  const [level, setLevel] = useState(() => (levels.length ? levelValue(levels[0]) : ""));
  const [dataset, setDataset] = useState("");
  const [deliveryKind, setDeliveryKind] = useState("");
  const [endpoint, setEndpoint] = useState("");
  const [owner, setOwner] = useState("");
  const [valuePerOutcome, setValuePerOutcome] = useState("");
  const [investment, setInvestment] = useState("");
  const [currency, setCurrency] = useState("");
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const levelNumber = parseInteger(level);
      const value = parseNumber(valuePerOutcome);
      const spent = parseNumber(investment);
      if (levelNumber === null || value === null || spent === null) throw new Refusal("Level and economics must be valid numbers.");
      const answer = await act<J>("start_solution", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        project: name,
        solution: text(solution, "id"),
        level: levelNumber,
        dataset,
        delivery: { kind: deliveryKind, endpoint, owner },
        roi_model: { value_per_outcome: value, investment: spent, currency },
      });
      navigate(ws(`/design/project?${qs({ name, task: text(answer, "task_id") })}`));
    });
  return (
    <Drawer
      title="Start This Solution"
      subtitle={`${text(solution, "id")} ${text(solution, "name")}`}
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="start-solution" variant="primary" loading={busy}>
            Start
          </Button>
        </>
      }
    >
      <Form id="start-solution" onSubmit={submit}>
        <FormError error={error} />
        <Field label="Project name" required>
          <input name="name" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        {levels.length ? (
          <Field label="Level">
            <select name="level" value={level} onChange={(e) => setLevel(e.target.value)}>
              {levels.map((l, index) => (
                <option key={index} value={levelValue(l)}>
                  {text(l, "name") || text(l, "id")}
                </option>
              ))}
            </select>
          </Field>
        ) : (
          <Field label="Level" required>
            <input name="level" type="number" min={1} max={4} value={level} onChange={(e) => setLevel(e.target.value)} />
          </Field>
        )}
        <Field label="Dataset" required error={datasetsError ? `The datasets could not be listed: ${(datasetsError as Error).message}` : undefined}>
          <select name="dataset" value={dataset} onChange={(e) => setDataset(e.target.value)}>
            <option value="" disabled>
              Choose a dataset
            </option>
            {datasets.map((path) => (
              <option key={path} value={path}>
                {path}
              </option>
            ))}
          </select>
        </Field>
        <div className="label mt-2">Where Actions Go</div>
        <Field label="Delivery" required>
          <select name="delivery_kind" value={deliveryKind} onChange={(e) => setDeliveryKind(e.target.value)}>
            <option value="" disabled>
              Choose delivery
            </option>
            <option value="webhook">Webhook</option>
            <option value="salesforce">Salesforce connector</option>
            <option value="hubspot">HubSpot connector</option>
          </select>
        </Field>
        <Field label="Webhook URL or connector endpoint" required>
          <input name="delivery_endpoint" value={endpoint} onChange={(e) => setEndpoint(e.target.value)} />
        </Field>
        <Field label="Owner" required>
          <input name="owner" value={owner} onChange={(e) => setOwner(e.target.value)} />
        </Field>
        <div className="label mt-2">Economics</div>
        <Field label="Value per outcome" required>
          <input name="value_per_outcome" type="number" step="any" value={valuePerOutcome} onChange={(e) => setValuePerOutcome(e.target.value)} />
        </Field>
        <Field label="Investment" required>
          <input name="investment" type="number" step="any" min={0} value={investment} onChange={(e) => setInvestment(e.target.value)} />
        </Field>
        <Field label="Currency" required help="These are your inputs. lake does not supply defaults.">
          <input name="currency" placeholder="USD" value={currency} onChange={(e) => setCurrency(e.target.value)} />
        </Field>
      </Form>
    </Drawer>
  );
}
