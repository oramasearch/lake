/** Design → Methodologies (lake's methodologies_content): the modelling methods the workspace's
 * engine knows, as cards grouped by family, with a search and a filter by family. */
import { useState } from "react";
import { FlaskConical, SearchX } from "lucide-react";
import { Card, Empty, FilterChips, Grid, Page, SearchInput, Section, Skeleton, Stat, StatRow, Toolbar } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useWq, useWsPath } from "./api";
import { joined, text, type J } from "./logic";
import { matches } from "./catalogue";
import { RunsBadge, useDesignTrail } from "./parts";

export const NO_METHODOLOGIES = "This workspace's engine does not list its methodologies yet. It runs tabular prediction (M-1).";

export default function MethodologiesPage() {
  useDesignTrail({ label: "Methodologies", path: "/design/methodologies" }, [], "Methodologies");
  return (
    <Page
      eyebrow="Design"
      title="Methodologies"
      sub="The modelling methods your workspace's engine knows: what each needs, what it makes, and whether it runs here today. Solutions compose them."
    >
      <WorkspaceGate>
        <Families />
      </WorkspaceGate>
    </Page>
  );
}

function Families() {
  const ws = useWsPath();
  const [search, setSearch] = useState("");
  const [family, setFamily] = useState<string | null>(null);
  // A failed read is not shown as an error: an older engine has no methodology registry.
  const listed = useWq<J>("list_methodologies", {}, { retry: false });
  if (listed.isLoading) return <Skeleton />;
  const entries: J[] = !listed.isError && Array.isArray(listed.data?.methodologies) ? listed.data!.methodologies : [];
  if (!entries.length) {
    return (
      <Empty icon={FlaskConical} title="No Methodology Registry Yet">
        {NO_METHODOLOGIES}
      </Empty>
    );
  }
  const counts = new Map<string, number>();
  for (const e of entries) counts.set(text(e, "family"), (counts.get(text(e, "family")) ?? 0) + 1);
  const shown = entries.filter((e) => (!family || text(e, "family") === family) && matches(e, search, ["id", "name", "family", "outputs", "inputs", "metrics"]));
  // Consecutive entries of one family share a section, as the server grouped them.
  const families: { family: string; entries: J[] }[] = [];
  for (const entry of shown) {
    const name = text(entry, "family");
    const last = families[families.length - 1];
    if (last && last.family === name) last.entries.push(entry);
    else families.push({ family: name, entries: [entry] });
  }
  const runnable = entries.filter((e) => e.runnable === true).length;
  return (
    <>
      <StatRow cols={3}>
        <Stat label="Methodologies" value={entries.length} sub="the engine knows" />
        <Stat label="Run here" value={runnable} tone={runnable ? "ok" : undefined} sub="today, on this engine" />
        <Stat label="Families" value={counts.size} />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search methodologies…" label="Search methodologies" />
        <FilterChips options={[...counts].map(([id, count]) => ({ id, label: id || "—", count }))} value={family} onChange={setFamily} allLabel="All families" />
      </Toolbar>
      {!families.length && (
        <Empty compact icon={SearchX} title="No Methodology Matches">
          Clear the search or the filter to see more.
        </Empty>
      )}
      {families.map((group, index) => (
        <Section key={index} title={group.family}>
          <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
            {group.entries.map((entry, row) => (
              <Card
                key={text(entry, "id") || row}
                to={ws(`/design/methodology?${qs({ id: text(entry, "id") })}`)}
                label={text(entry, "id")}
                title={text(entry, "name")}
                actions={<RunsBadge entry={entry} />}
                className="h-full"
              >
                <div className="text-[13px] text-ink-2">
                  <span className="text-ink-3">Makes </span>
                  {joined(entry.outputs) || "—"}
                </div>
                {joined(entry.inputs) && (
                  <div className="mt-1 line-clamp-2 text-[12.5px] text-ink-3">
                    Needs {joined(entry.inputs)}
                  </div>
                )}
              </Card>
            ))}
          </Grid>
        </Section>
      ))}
    </>
  );
}
