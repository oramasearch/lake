/** Design → Solution (lake's solutions_content, solution branch): one catalogue solution, its runs,
 * and Start This Solution (a drawer) when it runs or is in preview. Start parses the level and the
 * economics as the form handler did, and opens the new project following its foundation task. */
import { useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { Rocket } from "lucide-react";
import { act } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { Button, Empty, ErrorBlock, Field, Form, KeyValue, Page, Skeleton, Split } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath } from "./api";
import { datasetChoices, joined, parseInteger, parseNumber, runIsActive, slugText, text, type J } from "./logic";
import { MethodologyLinks, NoCatalogue } from "./catalogue";
import { FormError, Panel, Refusal, RunsBadge, RunsPanel, entityCrumb, useDesignTrail, useSubmit } from "./parts";
import "@/styles/design.css";

/** The solution in `get_solution`'s answer: the solution itself or {solution: {...}}. */
function solutionOf(entry: J | undefined): J | null {
  if (!entry || (!("id" in entry) && !("solution" in entry))) return null;
  const solution: J = entry.solution && typeof entry.solution === "object" ? entry.solution : entry;
  return text(solution, "id") ? solution : null;
}

const levelsOf = (solution: J): J[] => (Array.isArray(solution.capability_levels) ? solution.capability_levels : Array.isArray(solution.levels) ? solution.levels : []);

export default function SolutionPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [id ? entityCrumb(id) : { label: "Solution" }], id ? `Solution: ${id}` : "Solution");
  // The same read the body makes (one request): the header names the solution and offers Start.
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const got = useWq<J>("get_solution", { solution: id }, { retry: false, enabled: ready });
  const solution = solutionOf(got.data);
  const startable = !!solution && (solution.runnable === true || text(solution, "status") === "preview");
  const [starting, setStarting] = useState(false);
  return (
    <Page
      eyebrow={solution ? `Design · Solution ${text(solution, "id")}` : "Design · Solution"}
      title={
        solution ? (
          <span className="inline-flex flex-wrap items-center gap-2">
            {text(solution, "name") || id} <RunsBadge entry={solution} />
          </span>
        ) : (
          id || "Solution"
        )
      }
      sub={solution ? text(solution, "promise") : undefined}
      actions={
        startable ? (
          <Button variant="primary" onClick={() => setStarting(true)}>
            <Rocket size={14} aria-hidden /> Start this solution
          </Button>
        ) : undefined
      }
    >
      <WorkspaceGate>
        <SolutionBody key={id} id={id} starting={starting} onStarting={setStarting} />
      </WorkspaceGate>
    </Page>
  );
}

function SolutionBody({ id, starting, onStarting }: { id: string; starting: boolean; onStarting: (open: boolean) => void }) {
  const [live, setLive] = useState(false);
  const got = useWq<J>("get_solution", { solution: id }, { retry: false });
  const datasets = useWq<J>("list_datasets");
  const runs = useScopedRead<{ runs?: J[] }>("list_runs", { solution: id, limit: 5 }, { refetchInterval: live ? 3000 : false });
  const allRuns: J[] = Array.isArray(runs.data?.runs) ? runs.data!.runs! : [];
  const nowLive = allRuns.some(runIsActive);
  if (nowLive !== live) setLive(nowLive);

  if (got.isLoading) return <Skeleton />;
  // An engine without the catalogue answers unknown_capability: the catalogue note, not an error.
  if (got.isError && (got.error as { code?: string }).code === "unknown_capability") return <NoCatalogue />;
  if (got.isError) return <ErrorBlock error={got.error} retry={() => void got.refetch()} what="this solution" />;
  const entry = got.data ?? {};
  if (!("id" in entry) && !("solution" in entry)) return <NoCatalogue />;
  const solution = solutionOf(entry);
  if (!solution) return <Empty title={`No solution ${id}`}>The catalogue lists no solution by that id.</Empty>;
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
