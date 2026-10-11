/** Design → Methodology (lake's methodologies_content, methodology branch): one methodology, the
 * solutions that use it, and a way to build a pipeline with it when it runs. Main column: what it
 * needs, makes and is measured by, and its solutions; side: its status and the next step. */
import { FlaskConical, Workflow } from "lucide-react";
import { useSearchParams } from "react-router-dom";
import { Button, Card, Empty, Grid, KeyValue, Page, Skeleton, Split } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { MethodologyIcon, SolutionIcon } from "@/components/CatalogueIcon";
import { qs, useWq, useWsPath } from "./api";
import { joined, solutionUses, text, type J } from "./logic";
import { Panel, RunsBadge, entityCrumb, useDesignTrail } from "./parts";
import { NO_METHODOLOGIES } from "./MethodologiesPage";

export default function MethodologyPage() {
  const [params] = useSearchParams();
  const id = params.get("id") ?? "";
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [id ? entityCrumb(id) : { label: "Methodology" }], id ? `Methodology: ${id}` : "Methodology");
  // The same read the body makes (one request), for the icon beside the title.
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const listed = useWq<J>("list_methodologies", {}, { retry: false, enabled: ready });
  const entry = !listed.isError && Array.isArray(listed.data?.methodologies) ? (listed.data!.methodologies as J[]).find((e) => text(e, "id") === id) : undefined;
  return (
    <Page eyebrow="Design · Methodology" title={id || "Methodology"} icon={entry ? <MethodologyIcon entry={entry} size={64} /> : undefined}>
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
  if (!entries.length) {
    return (
      <Empty icon={FlaskConical} title="No Methodology Registry Yet">
        {NO_METHODOLOGIES}
      </Empty>
    );
  }
  const entry = entries.find((e) => text(e, "id") === id);
  if (!entry) {
    return (
      <Empty icon={FlaskConical} title={`No methodology ${id}`} action={<Button to={ws("/design/methodologies")}>All methodologies</Button>}>
        The engine lists no methodology by that id.
      </Empty>
    );
  }
  const users: J[] = (!solutions.isError && Array.isArray(solutions.data?.solutions) ? solutions.data!.solutions : []).filter((s: J) => solutionUses(s, id));
  const runnable = entry.runnable === true;
  return (
    <Split
      side={
        <>
          <Panel label="Status" title={<RunsBadge entry={entry} />}>
            <KeyValue
              rows={[
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
