/** Design → Methodologies (lake's methodologies_content): the modelling methods the workspace's
 * engine knows, one panel per family. */
import { Link } from "react-router-dom";
import { Page, Skeleton } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWq, useWsPath } from "./api";
import { joined, text, type J } from "./logic";
import { Panel, RunsBadge, useDesignTrail } from "./parts";

export const NO_METHODOLOGIES = "This workspace's engine does not list its methodologies yet. It runs tabular prediction (M-1).";

export default function MethodologiesPage() {
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [], "Methodologies");
  return (
    <Page
      eyebrow="Design"
      title="Methodologies"
      sub="The modelling methods your workspace's engine knows: what each needs, what it makes, how it is measured, and whether it runs here today. Solutions compose them."
    >
      <WorkspaceGate>
        <Families />
      </WorkspaceGate>
    </Page>
  );
}

function Families() {
  const ws = useWsPath();
  // A failed read is not shown as an error: an older engine has no methodology registry.
  const listed = useWq<J>("list_methodologies", {}, { retry: false });
  if (listed.isLoading) return <Skeleton />;
  const entries: J[] = !listed.isError && Array.isArray(listed.data?.methodologies) ? listed.data!.methodologies : [];
  if (!entries.length) return <div className="panel empty">{NO_METHODOLOGIES}</div>;
  // Consecutive entries of one family share a panel, as the server grouped them.
  const families: { family: string; entries: J[] }[] = [];
  for (const entry of entries) {
    const family = text(entry, "family");
    const last = families[families.length - 1];
    if (last && last.family === family) last.entries.push(entry);
    else families.push({ family, entries: [entry] });
  }
  return (
    <>
      {families.map((group, index) => (
        <Panel key={index} title={group.family}>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Methodology</th>
                  <th>What it makes</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                {group.entries.map((entry, row) => (
                  <tr key={text(entry, "id") || row}>
                    <td>
                      <Link to={ws(`/design/methodology?${qs({ id: text(entry, "id") })}`)}>{`${text(entry, "id")} ${text(entry, "name")}`}</Link>
                    </td>
                    <td>{joined(entry.outputs)}</td>
                    <td>
                      <RunsBadge entry={entry} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Panel>
      ))}
    </>
  );
}
