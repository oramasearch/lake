/** Design → Solutions (lake's solutions_content): the engine's solutions catalogue as cards, modules
 * first, then what is still in design, with a search and a filter by whether each runs here. */
import { useState } from "react";
import { SearchX } from "lucide-react";
import { Card, Empty, FilterChips, Grid, Page, SearchInput, Section, Skeleton, Stat, StatRow, Toolbar } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWq, useWsPath } from "./api";
import { runsBadge, text, type J } from "./logic";
import { MethodologyChips, NoCatalogue, matches } from "./catalogue";
import { RunsBadge, useDesignTrail } from "./parts";

export default function SolutionsPage() {
  useDesignTrail({ label: "Solutions", path: "/design/solutions" }, [], "Solutions");
  return (
    <Page
      eyebrow="Design"
      title="Solutions"
      sub="Orama's solutions catalogue: each module answers one business problem end to end. Start one and it becomes a project in your workspace."
    >
      <WorkspaceGate>
        <Catalogue />
      </WorkspaceGate>
    </Page>
  );
}

function Catalogue() {
  const [search, setSearch] = useState("");
  const [state, setState] = useState<string | null>(null);
  // A failed catalogue read is not shown as an error: the engine may not list solutions yet.
  const listed = useWq<J>("list_solutions", {}, { retry: false });
  if (listed.isLoading) return <Skeleton />;
  if (listed.isError || !listed.data || !("solutions" in listed.data)) return <NoCatalogue />;
  const entries: J[] = Array.isArray(listed.data.solutions) ? listed.data.solutions : [];
  if (!entries.length) return <Empty title="No Solutions Listed">The engine's catalogue is empty.</Empty>;

  const labels = new Map<string, number>();
  for (const e of entries) labels.set(runsBadge(e).label, (labels.get(runsBadge(e).label) ?? 0) + 1);
  const shown = entries.filter((e) => (!state || runsBadge(e).label === state) && matches(e, search, ["id", "name", "promise", "problem", "use_case"]));
  const modules = shown.filter((e) => text(e, "status") !== "sketch");
  const sketches = shown.filter((e) => text(e, "status") === "sketch");
  const runsToday = entries.filter((e) => e.runnable === true).length;

  return (
    <>
      <StatRow cols={3}>
        <Stat label="Solutions" value={entries.length} sub="in the catalogue" />
        <Stat label="Run here" value={runsToday} tone={runsToday ? "ok" : undefined} sub="on this workspace's engine" />
        <Stat label="In design" value={entries.filter((e) => text(e, "status") === "sketch").length} sub="not yet a module" />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search solutions…" label="Search solutions" />
        <FilterChips options={[...labels].map(([id, count]) => ({ id, label: id, count }))} value={state} onChange={setState} />
      </Toolbar>
      {!modules.length && !sketches.length && (
        <Empty compact icon={SearchX} title="No Solution Matches">
          Clear the search or a filter to see more.
        </Empty>
      )}
      {modules.length > 0 && (
        <Section title="Modules">
          <SolutionCards entries={modules} />
        </Section>
      )}
      {sketches.length > 0 && (
        <Section title="In Design">
          <SolutionCards entries={sketches} />
        </Section>
      )}
    </>
  );
}

function SolutionCards({ entries }: { entries: J[] }) {
  const ws = useWsPath();
  return (
    <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
      {entries.map((entry, index) => (
        <Card
          key={text(entry, "id") || index}
          to={ws(`/design/solution?${qs({ id: text(entry, "id") })}`)}
          label={text(entry, "id")}
          title={text(entry, "name")}
          actions={<RunsBadge entry={entry} />}
          className="h-full"
        >
          <p className="m-0 line-clamp-3 text-[13px] text-ink-2">{text(entry, "promise") || "—"}</p>
          <div className="mt-2.5">
            <MethodologyChips solution={entry} />
          </div>
        </Card>
      ))}
    </Grid>
  );
}
