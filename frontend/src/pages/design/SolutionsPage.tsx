/** Design → Solutions (lake's solutions_content): the engine's solutions catalogue, modules first,
 * then what is still in design. */
import { Link } from "react-router-dom";
import { Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWq, useWsPath } from "./api";
import { text, type J } from "./logic";
import { MethodologyLinks, NoCatalogue } from "./catalogue";
import { Panel, RunsBadge, useDesignTrail } from "./parts";

export default function SolutionsPage() {
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [], "Solutions");
  return (
    <Page
      eyebrow="Design"
      title="Solutions"
      sub="Orama's solutions catalogue: each module answers one business problem end to end, from the methodologies it composes to the decision it drives and how its return is measured. Start one and it becomes a project in your workspace."
    >
      <WorkspaceGate>
        <Catalogue />
      </WorkspaceGate>
    </Page>
  );
}

function Catalogue() {
  // A failed catalogue read is not shown as an error: the engine may not list solutions yet.
  const listed = useWq<J>("list_solutions", {}, { retry: false });
  if (listed.isLoading) return <Skeleton />;
  if (listed.isError || !listed.data || !("solutions" in listed.data)) return <NoCatalogue />;
  const entries: J[] = Array.isArray(listed.data.solutions) ? listed.data.solutions : [];
  const modules = entries.filter((e) => text(e, "status") !== "sketch");
  const sketches = entries.filter((e) => text(e, "status") === "sketch");
  if (!modules.length && !sketches.length) return <div className="panel empty">No solutions listed.</div>;
  return (
    <>
      {modules.length > 0 && (
        <Panel title="Modules">
          <SolutionTable entries={modules} />
        </Panel>
      )}
      {sketches.length > 0 && (
        <Panel title="In Design">
          <SolutionTable entries={sketches} />
        </Panel>
      )}
    </>
  );
}

function SolutionTable({ entries }: { entries: J[] }) {
  const ws = useWsPath();
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Solution</th>
            <th>What it does</th>
            <th>Methodologies</th>
            <th>Status</th>
          </tr>
        </thead>
        <tbody>
          {entries.map((entry, index) => (
            <tr key={text(entry, "id") || index}>
              <td>
                <Link to={ws(`/design/solution?${qs({ id: text(entry, "id") })}`)}>{`${text(entry, "id")} ${text(entry, "name")}`}</Link>
              </td>
              <td>{text(entry, "promise")}</td>
              <td>
                <MethodologyLinks solution={entry} />
              </td>
              <td>
                <RunsBadge entry={entry} />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
