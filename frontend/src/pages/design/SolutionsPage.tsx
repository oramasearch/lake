/** Design → Solutions (lake#597): Orama's solutions catalogue, read-only, and the workspace's own
 * solutions, which a person creates, edits and deletes, each card marked "Orama" or "Yours". A
 * toolbar filters by who a solution is from, by whether Orama's runs here, and by a search; New
 * Solution opens a drawer. One read, lake's `list_solutions`, answers both kinds. */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { Lightbulb, Plus, SearchX } from "lucide-react";
import { act } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { Badge, Banner, Button, Card, Empty, ErrorBlock, FilterChips, Grid, Page, SearchInput, Section, Skeleton, Stat, StatRow, Toolbar } from "@/lib/ui";
import { WorkspaceGate, useWorkspaceStatus } from "@/components/WorkspaceGate";
import { SolutionIcon } from "@/components/CatalogueIcon";
import { qs, useScope, useScopedRead, useWsPath } from "./api";
import { filterSolutions, runsBadge, solutionForm, solutionInput, solutionSource, sourceLabel, text, type J, type SolutionSource } from "./logic";
import { MethodologyChips, useMethodologyFamilies } from "./catalogue";
import { RunsBadge, useDesignTrail, useSubmit } from "./parts";
import { SolutionFields } from "./SolutionForm";

type Listed = { solutions?: J[]; catalogue?: { available?: boolean; message?: string | null } };

export default function SolutionsPage() {
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [], "Solutions");
  const ready = useWorkspaceStatus().q.data?.ready === true;
  const [creating, setCreating] = useState(false);
  return (
    <Page
      eyebrow="Design"
      title="Solutions"
      sub="Each solution answers one business problem end to end. Start Orama's, or write your own, and it becomes a project in your workspace."
      actions={
        ready ? (
          <Button variant="primary" onClick={() => setCreating(true)}>
            <Plus size={14} aria-hidden /> New solution
          </Button>
        ) : undefined
      }
    >
      <WorkspaceGate>
        <Solutions onNew={() => setCreating(true)} />
        {creating && <NewSolution onClose={() => setCreating(false)} />}
      </WorkspaceGate>
    </Page>
  );
}

function Solutions({ onNew }: { onNew: () => void }) {
  const [search, setSearch] = useState("");
  const [source, setSource] = useState<SolutionSource | null>(null);
  const [state, setState] = useState<string | null>(null);
  const listed = useScopedRead<Listed>("list_solutions");
  if (listed.isLoading) return <Skeleton />;
  if (listed.isError) return <ErrorBlock error={listed.error} retry={() => void listed.refetch()} what="solutions" />;
  const entries: J[] = Array.isArray(listed.data?.solutions) ? listed.data!.solutions! : [];
  const catalogue = listed.data?.catalogue;
  const orama = entries.filter((e) => solutionSource(e) === "orama");
  const yours = entries.filter((e) => solutionSource(e) === "workspace");

  const states = new Map<string, number>();
  for (const e of orama) states.set(runsBadge(e).label, (states.get(runsBadge(e).label) ?? 0) + 1);
  const shown = filterSolutions(entries, { source, state, search });
  const shownYours = shown.filter((e) => solutionSource(e) === "workspace");
  const modules = shown.filter((e) => solutionSource(e) === "orama" && text(e, "status") !== "sketch");
  const sketches = shown.filter((e) => solutionSource(e) === "orama" && text(e, "status") === "sketch");
  const runsToday = orama.filter((e) => e.runnable === true).length;
  const filtering = !!search.trim() || !!source || !!state;

  return (
    <>
      <StatRow cols={3}>
        <Stat label="Orama's" value={orama.length} sub="in the catalogue, read-only" />
        <Stat label="Yours" value={yours.length} sub="written in this workspace" />
        <Stat label="Run here" value={runsToday} tone={runsToday ? "ok" : undefined} sub="of Orama's, on this workspace's engine" />
      </StatRow>
      {catalogue?.available === false && (
        <Banner tone="quiet" title="Orama's Solutions Are Not Listed">
          {catalogue.message || "This workspace's engine does not list Orama's solutions yet."} Your own solutions are listed below.
        </Banner>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search solutions…" label="Search solutions" />
        <FilterChips<SolutionSource>
          allLabel="All"
          options={[
            { id: "orama", label: "Orama", count: orama.length },
            { id: "workspace", label: "Yours", count: yours.length },
          ]}
          value={source}
          onChange={setSource}
        />
        {states.size > 0 && source !== "workspace" && (
          <FilterChips options={[...states].map(([id, count]) => ({ id, label: id, count }))} value={state} onChange={setState} allLabel="Any state" />
        )}
      </Toolbar>
      {filtering && !shown.length && (
        <Empty compact icon={SearchX} title="No Solution Matches">
          Clear the search or a filter to see more.
        </Empty>
      )}
      {(source === null || source === "workspace") && !state && (shownYours.length > 0 || !filtering) && (
        <Section title="Yours">
          {shownYours.length ? (
            <SolutionCards entries={shownYours} />
          ) : (
            <Empty
              compact
              icon={Lightbulb}
              title="No Solutions of Your Own Yet"
              action={
                <Button variant="primary" onClick={onNew}>
                  <Plus size={14} aria-hidden /> New solution
                </Button>
              }
            >
              Write one for a problem the catalogue does not cover, or open one of Orama's and press Duplicate to edit.
            </Empty>
          )}
        </Section>
      )}
      {modules.length > 0 && (
        <Section title="Orama's Modules">
          <SolutionCards entries={modules} />
        </Section>
      )}
      {sketches.length > 0 && (
        <Section title="Orama's, in Design">
          <SolutionCards entries={sketches} />
        </Section>
      )}
    </>
  );
}

function SolutionCards({ entries }: { entries: J[] }) {
  const ws = useWsPath();
  const families = useMethodologyFamilies();
  return (
    <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
      {entries.map((entry, index) => {
        const own = solutionSource(entry) === "workspace";
        return (
          <Card
            key={text(entry, "id") || index}
            to={ws(`/design/solution?${qs({ id: text(entry, "id") })}`)}
            icon={<SolutionIcon entry={entry} size={32} families={families} />}
            label={own ? undefined : text(entry, "id")}
            title={text(entry, "name")}
            actions={
              <span className="inline-flex flex-wrap items-center gap-1">
                <Badge tone={own ? "success" : "info"} title={own ? "Your workspace's own: you can edit it" : "Orama's catalogue: read-only"}>
                  {sourceLabel(entry)}
                </Badge>
                {!own && <RunsBadge entry={entry} />}
              </span>
            }
            className="h-full"
          >
            <p className="m-0 line-clamp-3 text-[13px] text-ink-2">{(own ? text(entry, "decision") || text(entry, "problem") : text(entry, "promise")) || "—"}</p>
            <div className="mt-2.5">
              <MethodologyChips solution={entry} />
            </div>
          </Card>
        );
      })}
    </Grid>
  );
}

/** New Solution: the catalogue's shape as far as a person writes it; the new solution opens. */
function NewSolution({ onClose }: { onClose: () => void }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [form, setForm] = useState(() => solutionForm(null));
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const answer = await act<{ solution?: J }>("create_solution", { customer_id: scope.customerId, workspace_id: scope.workspaceId, ...solutionInput(form) });
      navigate(ws(`/design/solution?${qs({ id: text(answer.solution, "id") })}`));
    });
  return (
    <Drawer
      title="New Solution"
      subtitle="The business problem, the decision it drives and how its return is measured."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button type="submit" form="new-solution" variant="primary" loading={busy}>
            Create solution
          </Button>
        </>
      }
    >
      <SolutionFields id="new-solution" form={form} onChange={setForm} onSubmit={submit} error={error} />
    </Drawer>
  );
}
