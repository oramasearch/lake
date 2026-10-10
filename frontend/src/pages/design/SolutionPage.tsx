/** Design → Solution (lake's solutions_content, solution branch): one catalogue solution, its runs,
 * and Start This Solution when it runs or is in preview. Start parses the level and the economics
 * as the form handler did, and opens the new project following its foundation task. */
import { useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { act } from "@/lib/api";
import { Button, ErrorBlock, Field, Form, Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useScope, useScopedRead, useWq, useWsPath } from "./api";
import { datasetChoices, joined, parseInteger, parseNumber, runIsActive, slugText, text, type J } from "./logic";
import { MethodologyLinks, NoCatalogue } from "./catalogue";
import { FormError, Panel, Refusal, RunsBadge, RunsPanel, entityCrumb, useDesignTrail, useSubmit } from "./parts";

export default function SolutionPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [id ? entityCrumb(id) : { label: "Solution" }], id ? `Solution: ${id}` : "Solution");
  return (
    <Page eyebrow="Design" title={id ? `Solution: ${id}` : "Solution"}>
      <WorkspaceGate>
        <SolutionBody key={id} id={id} />
      </WorkspaceGate>
    </Page>
  );
}

function SolutionBody({ id }: { id: string }) {
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
  // The answer is the solution itself or {solution: {...}}.
  const solution: J = entry.solution && typeof entry.solution === "object" ? entry.solution : entry;
  if (!text(solution, "id")) return <div className="panel empty">No solution {id}.</div>;
  const levels: J[] = Array.isArray(solution.capability_levels) ? solution.capability_levels : Array.isArray(solution.levels) ? solution.levels : [];
  const runnable = solution.runnable === true;
  const startable = runnable || text(solution, "status") === "preview";
  return (
    <>
      <p className="sub">
        {text(solution, "id")} · <RunsBadge entry={solution} />
      </p>
      <section className="panel">
        <p>{text(solution, "promise")}</p>
        <div className="table-wrap">
          <table>
            <tbody>
              <tr>
                <th>The problem</th>
                <td>{text(solution, "problem") || text(solution, "use_case")}</td>
              </tr>
              <tr>
                <th>Methodologies</th>
                <td>
                  <MethodologyLinks solution={solution} />
                </td>
              </tr>
              <tr>
                <th>How it acts</th>
                <td>{joined(solution.action_patterns)}</td>
              </tr>
              <tr>
                <th>Decision speed</th>
                <td>{text(solution, "latency") || joined(solution.latency_classes)}</td>
              </tr>
              <tr>
                <th>What data it needs</th>
                <td>{joined(Array.isArray(solution.required_data_roles) ? solution.required_data_roles : solution.data_roles)}</td>
              </tr>
              {!runnable && text(solution, "issue") && (
                <tr>
                  <th>Tracked in</th>
                  <td>{text(solution, "issue")}</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
        {levels.length > 0 && (
          <>
            <h2>Levels</h2>
            <ul>
              {levels.map((level, index) => (
                <li key={index}>
                  <strong>{text(level, "name") || text(level, "id")}</strong> {text(level, "description")}
                </li>
              ))}
            </ul>
          </>
        )}
      </section>
      {runs.isError ? <ErrorBlock error={runs.error} retry={() => void runs.refetch()} what="runs" /> : runs.isLoading ? <Skeleton /> : <RunsPanel runs={allRuns} />}
      {startable && <StartSolution solution={solution} levels={levels} datasets={datasetChoices(datasets.data?.datasets).map((d) => d.path)} datasetsError={datasets.error} />}
    </>
  );
}

/** Start This Solution (lake#292): the person supplies the business inputs; the Project Agent
 * proposes the rest of the foundation, and nothing is created until it is confirmed. */
function StartSolution({ solution, levels, datasets, datasetsError }: { solution: J; levels: J[]; datasets: string[]; datasetsError: unknown }) {
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
    <Panel title="Start This Solution">
      <Form onSubmit={submit}>
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
        <fieldset>
          <legend>Where Actions Go</legend>
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
        </fieldset>
        <fieldset>
          <legend>Economics</legend>
          <Field label="Value per outcome" required>
            <input name="value_per_outcome" type="number" step="any" value={valuePerOutcome} onChange={(e) => setValuePerOutcome(e.target.value)} />
          </Field>
          <Field label="Investment" required>
            <input name="investment" type="number" step="any" min={0} value={investment} onChange={(e) => setInvestment(e.target.value)} />
          </Field>
          <Field label="Currency" required>
            <input name="currency" placeholder="USD" value={currency} onChange={(e) => setCurrency(e.target.value)} />
          </Field>
          <small>These are your inputs. lake does not supply defaults.</small>
        </fieldset>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Start
          </Button>
        </div>
      </Form>
    </Panel>
  );
}
