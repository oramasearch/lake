/** Design → Methodology (lake's methodologies_content, methodology branch): one methodology, the
 * solutions that use it, and a way to build a pipeline with it when it runs. */
import { Fragment } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Button, Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWq, useWsPath } from "./api";
import { joined, solutionUses, text, type J } from "./logic";
import { RunsBadge, entityCrumb, useDesignTrail } from "./parts";
import { NO_METHODOLOGIES } from "./MethodologiesPage";

export default function MethodologyPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [id ? entityCrumb(id) : { label: "Methodology" }], id ? `Methodology: ${id}` : "Methodology");
  return (
    <Page eyebrow="Design" title={id ? `Methodology: ${id}` : "Methodology"}>
      <WorkspaceGate>
        <Methodology id={id} />
      </WorkspaceGate>
    </Page>
  );
}

function Methodology({ id }: { id: string }) {
  const ws = useWsPath();
  // Failures are not shown: an older engine lists neither.
  const methodologies = useWq<J>("list_methodologies", {}, { retry: false });
  const solutions = useWq<J>("list_solutions", {}, { retry: false });
  if (methodologies.isLoading || solutions.isLoading) return <Skeleton />;
  const entries: J[] = !methodologies.isError && Array.isArray(methodologies.data?.methodologies) ? methodologies.data!.methodologies : [];
  if (!entries.length) return <div className="panel empty">{NO_METHODOLOGIES}</div>;
  const entry = entries.find((e) => text(e, "id") === id);
  if (!entry) return <div className="panel empty">No methodology {id}.</div>;
  const users: J[] = (!solutions.isError && Array.isArray(solutions.data?.solutions) ? solutions.data!.solutions : []).filter((s: J) => solutionUses(s, id));
  const runnable = entry.runnable === true;
  return (
    <>
      <p className="sub">
        {text(entry, "family")} · <RunsBadge entry={entry} />
      </p>
      <section className="panel">
        <div className="table-wrap">
          <table>
            <tbody>
              <tr>
                <th>What it needs</th>
                <td>{joined(entry.inputs)}</td>
              </tr>
              <tr>
                <th>What it makes</th>
                <td>{joined(entry.outputs)}</td>
              </tr>
              <tr>
                <th>How it is measured</th>
                <td>{joined(entry.metrics)}</td>
              </tr>
              <tr>
                <th>Model kinds</th>
                <td>{joined(entry.model_kinds)}</td>
              </tr>
              <tr>
                <th>Solutions that use it</th>
                <td>
                  {users.length
                    ? users.map((s, index) => (
                        <Fragment key={text(s, "id") || index}>
                          {index > 0 && ", "}
                          <Link to={ws(`/design/solution?${qs({ id: text(s, "id") })}`)}>{`${text(s, "id")} ${text(s, "name")}`}</Link>
                        </Fragment>
                      ))
                    : "—"}
                </td>
              </tr>
              {!runnable && text(entry, "issue") && (
                <tr>
                  <th>Tracked in</th>
                  <td>{text(entry, "issue")}</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </section>
      {runnable && (
        <p>
          <Button variant="primary" to={ws("/design/projects")}>
            Build a pipeline with it
          </Button>{" "}
          <span className="sub">Open a project and choose {id} under Build a Pipeline.</span>
        </p>
      )}
    </>
  );
}
